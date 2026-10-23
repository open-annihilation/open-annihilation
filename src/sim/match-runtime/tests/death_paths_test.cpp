// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The ways a unit dies: its death blast (through the detonation, with its
// screen shake and sounds), the player sweep of the commander rule on units
// simulated here and elsewhere, which machine runs that sweep and who counts
// as a commander, the deaths dealt through the damage path, the corpse
// smoke the kill handler asks for, and the instance generation of the unit
// made next in the dead unit's slot.
#include "match_tick_access.hpp"
#include "combat_fixture.hpp"
#include "oa/data/unit_definitions.hpp"
#include "oa/sim/weapon_execution/weapon_launch.hpp"
#include "oa/base/text.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace combat_fixture;

constexpr uint8_t self_destruct =
    static_cast<uint8_t>(sim::match_runtime::DeathKind::self_destruct);
constexpr uint8_t captured = static_cast<uint8_t>(sim::match_runtime::DeathKind::captured);
constexpr uint8_t cargo_death = static_cast<uint8_t>(sim::match_runtime::DeathKind::cargo);
constexpr int16_t full_health = 1000;
constexpr uint8_t camera_shaking = 0x01; // Game.camera_flags

// Blast weapons with edge effectiveness 1, so every unit inside half the
// blast width takes the whole [DAMAGE] default. TESTBLAST shakes the screen
// by 6 for half a second and has impact sounds; TESTSELFD has neither.
constexpr std::string_view blast_tdf = R"([TESTBLAST]
{
ID=2; areaofeffect=96; edgeeffectiveness=1; explosiongaf=fx; explosionart=explode4;
soundhit=xplomed2; soundwater=splshbig; shakemagnitude=6; shakeduration=0.5;
[DAMAGE] { default=300; }
}
[TESTSELFD]
{
ID=3; areaofeffect=128; edgeeffectiveness=1; explosiongaf=fx; explosionart=explode5;
[DAMAGE] { default=500; }
}
)";

// The explode4 and explode5 pictures, each a few frames long so a blast is
// still animating when the tick that logged it ends.
struct BlastArt {
    formats::gaf::Sequence explode4 = frames(4);
    formats::gaf::Sequence explode5 = frames(4);

    static formats::gaf::Sequence frames(std::size_t count) {
        formats::gaf::Sequence sequence{};
        sequence.frames.resize(count);
        for (auto& frame : sequence.frames)
            frame.duration = 2;
        return sequence;
    }

    Options options() const {
        Options options;
        options.effect_sequence =
            [this](std::string_view, std::string_view entry) -> const formats::gaf::Sequence* {
            if (entry == "explode4")
                return &explode4;
            return entry == "explode5" ? &explode5 : nullptr;
        };
        return options;
    }
};

void arm_death_blasts(Fixture& f) {
    CHECK(sim::combat_state::install_weapon_text(f.weapons, blast_tdf) == 2);
    std::copy(
        f.weapons.records().begin(),
        f.weapons.records().end(),
        std::begin(f.match->state().game.weapon_defs)
    );
    f.def.explode_as = "TESTBLAST";
    f.def.self_destruct_as = "TESTSELFD";
}

// A unit that neither fires on its own nor chases.
sim::unit_spawn::Slot& idle(Fixture& f, uint8_t player, uint32_t x, uint32_t z) {
    auto& slot = f.spawn(player, x, z);
    slot.unit->flags &= ~OA_UNIT_FLAG_FIRE_ORDER_MASK;
    return slot;
}

bool live(const sim::unit_spawn::Slot& slot) {
    return (slot.unit->flags & OA_UNIT_FLAG_LIVE) != 0;
}

bool dying(const sim::unit_spawn::Slot& slot) {
    return (slot.unit->flags & OA_UNIT_FLAG_DEATH_PENDING) != 0;
}

// A full-health unit after the damage path scales `amount` by its veteran level:
// (25 - min(veteran / 5, 5)) * amount * 4 / 100.
int16_t after_hit(int32_t amount, uint16_t veteran) {
    const auto steps = std::min<int32_t>(veteran / 5, 5);
    return static_cast<int16_t>(full_health - (25 - steps) * amount * 4 / 100);
}

// Clips the match played through its point-sound hook.
struct HeardClips {
    std::vector<std::pair<std::string, FixedVec3>> clips;
};

void listen(Fixture& f, HeardClips& heard) {
    f.match->point_sound = {
        &heard,
        nullptr,
        [](void* context, const char* name, const sim::match_runtime::Match::PointSound& sound) {
            static_cast<HeardClips*>(context)->clips.emplace_back(name, sound.at);
        }
    };
}

bool same_point(const FixedVec3& left, const FixedVec3& right) {
    return left.x == right.x && left.y == right.y && left.z == right.z;
}

const WeaponDef& canonical_weapon(Fixture& f, std::string_view name) {
    const auto* definition = f.weapons.find(name);
    CHECK(definition != nullptr);
    return f.match->state().game.weapon_defs[definition->registry_index];
}

// A unit killed by a weapon explodes with its explodeas weapon once it has
// left its plot. The area blast measures each unit's box edge from the blast
// and damages those nearer than half the width (48), through the damage path
// with no source; the unit whose edge is exactly 48 away is untouched.
void death_blast_damages_neighbours() {
    const BlastArt art;
    Fixture f(art.options());
    arm_death_blasts(f);
    auto& victim = idle(f, 0, 64, 64);
    auto& ally = idle(f, 0, 80, 64);     // edge 8 away
    auto& enemy = idle(f, 1, 64, 104);   // edge 32 away
    auto& veteran = idle(f, 1, 104, 64); // edge 32 away
    auto& rim = idle(f, 1, 120, 64);     // edge 48 away
    veteran.record.veteran_level = 12;
    f.run(1);
    for (const auto* slot : {&ally, &enemy, &veteran, &rim})
        CHECK(slot->record.position.y == victim.record.position.y);
    const auto& effects = std::as_const(*f.match).effects();
    const auto blasts = effects.explosion_count;

    f.match->apply_damage_event(victim, nullptr, 30000, weapon_hit, 0);
    CHECK(dying(victim));
    f.run(1);
    CHECK(!live(victim));
    CHECK(effects.explosion_count == blasts + 1);
    CHECK(effects.explosions[blasts].sprite.sequence == &art.explode4);
    CHECK(ally.unit->health == after_hit(300, 0) && ally.unit->health == 700);
    CHECK(enemy.unit->health == 700);
    CHECK(veteran.unit->health == after_hit(300, 12) && veteran.unit->health == 724);
    CHECK(rim.unit->health == full_health);
    // The blast has no source, so nobody is credited with the hits.
    CHECK(ally.record.last_attacker_id == 0 && enemy.record.last_attacker_id == 0);
    // It shook the screen.
    const auto& game = f.match->state().game;
    CHECK(game.shake_duration == 7 && game.shake_amplitude_x == 6 && game.shake_amplitude_y == 6);
    std::cout << "death blast damages neighbours passed\n";
}

// The sweep of a player simulated here: each live unit takes 30000
// self-destruct damage from itself, scaled by its veteran level, and is left
// dying for its own update; a unit already dying is skipped.
void sweep_damages_local_units() {
    Fixture f;
    auto& plain = idle(f, 0, 64, 64);
    auto& veteran = idle(f, 0, 64, 160);
    auto& doomed = idle(f, 0, 160, 64);
    auto& enemy = idle(f, 1, 200, 200);
    veteran.record.veteran_level = 30;
    f.run(1);
    doomed.unit->flags |= OA_UNIT_FLAG_DEATH_PENDING;

    sim::match_runtime::MatchTickAccess host(*f.match);
    host.destroy_player_units(0);
    CHECK(plain.unit->health == after_hit(30000, 0) && plain.unit->health == -29000);
    CHECK(veteran.unit->health == after_hit(30000, 30) && veteran.unit->health == -23000);
    CHECK(dying(plain) && dying(veteran));
    CHECK(
        plain.record.last_attacker_id == plain.unit_index && plain.record.last_attacker_owner == 0
    );
    CHECK(plain.record.damage_kind == self_destruct);
    CHECK(doomed.unit->health == full_health);
    CHECK(enemy.unit->health == full_health && !dying(enemy));

    f.run(1);
    CHECK(!live(plain) && !live(veteran) && !live(doomed));
    CHECK(live(enemy));
    CHECK(f.match->state().game.players[0].unit_count == 0);
    std::cout << "sweep damages local units passed\n";
}

// A player with no units left is not swept, whatever its range holds.
void sweep_of_empty_player_returns() {
    Fixture f;
    auto& unit = idle(f, 0, 64, 64);
    f.run(1);
    auto& player = f.match->state().game.players[0];
    CHECK(player.unit_count == 1);
    player.unit_count = 0;
    sim::match_runtime::MatchTickAccess host(*f.match);
    host.destroy_player_units(0);
    CHECK(unit.unit->health == full_health && !dying(unit));
    player.unit_count = 1;
    std::cout << "sweep of empty player returns passed\n";
}

// The sweep of a player simulated elsewhere: each unit detonates its
// selfdestructas weapon at once, which damages nothing here, is marked dying
// and killed through the kill handler as a self-destruct. A unit that had already
// dropped to 0 health also gets a Killed percentage of at least 1 there and
// explodes a second time in the kill handler.
void sweep_kills_mirrored_units() {
    const BlastArt art;
    Fixture f(art.options());
    arm_death_blasts(f);
    auto& whole = idle(f, 1, 64, 64);
    auto& wrecked = idle(f, 1, 160, 160);
    auto& local = idle(f, 0, 80, 64);
    f.run(1);
    f.match->simulation().players[1].status = OA_PLAYER_STATUS_MIRRORED;
    wrecked.unit->health = 0;
    const auto& effects = std::as_const(*f.match).effects();
    const auto first = effects.explosion_count;
    auto& owner = f.match->state().game.players[1];
    const auto losses = owner.losses;

    sim::match_runtime::MatchTickAccess host(*f.match);
    host.destroy_player_units(1);
    CHECK(!live(whole) && !live(wrecked));
    CHECK(owner.unit_count == 0);
    CHECK(owner.losses == losses + 2);
    CHECK(effects.explosion_count == first + 3);
    const auto* blasts = effects.explosions + first;
    for (int blast = 0; blast < 3; ++blast)
        CHECK(blasts[blast].sprite.sequence == &art.explode5);
    CHECK(blasts[0].position.x == 64 << 16);
    CHECK(blasts[1].position.x == 160 << 16 && blasts[2].position.x == 160 << 16);
    CHECK(local.unit->health == full_health);
    std::cout << "sweep kills mirrored units passed\n";
}

// Order-panel closes the kill handler asked for, and whether the sweep had
// already reached `watched` at the first.
struct PanelCloses {
    const sim::unit_spawn::Slot* watched{};
    int count = 0;
    bool swept_first = false;
};

void watch_panel(Fixture& f, PanelCloses& closes) {
    f.match->order_panel = {&closes, [](void* context) {
                                auto& c = *static_cast<PanelCloses*>(context);
                                if (c.count++ == 0 && c.watched != nullptr)
                                    c.swept_first = dying(*c.watched);
                            }};
}

// The kill handler runs the commander rule's sweep only for a commander
// simulated here, after closing the order panel; the machine
// simulating a mirrored commander sweeps its player. The commander is the
// type side 0 names, the side of every setup record here.
void only_local_commander_sweeps() {
    Fixture f;
    f.match->state().game.session_rules = 1;
    oa::base::text::copy_terminated(f.match->state().game.sides[0].commander, "testunit");
    auto& mirrored_commander = idle(f, 1, 64, 64);
    auto& mirrored_unit = idle(f, 1, 64, 160);
    auto& local_commander = idle(f, 0, 160, 64);
    auto& local_unit = idle(f, 0, 160, 160);
    f.run(1);
    f.match->simulation().players[1].status = OA_PLAYER_STATUS_MIRRORED;
    PanelCloses closes{&local_unit};
    watch_panel(f, closes);

    sim::match_runtime::MatchTickAccess host(*f.match);
    mirrored_commander.unit->flags |= OA_UNIT_FLAG_DEATH_PENDING;
    host.kill_unit(mirrored_commander.record, weapon_hit);
    CHECK(!live(mirrored_commander));
    CHECK(
        live(mirrored_unit) && !dying(mirrored_unit) && mirrored_unit.unit->health == full_health
    );
    CHECK(closes.count == 0);

    local_commander.unit->flags |= OA_UNIT_FLAG_DEATH_PENDING;
    host.kill_unit(local_commander.record, weapon_hit);
    CHECK(!live(local_commander));
    CHECK(dying(local_unit) && local_unit.unit->health == after_hit(30000, 0));
    CHECK(closes.count == 1 && !closes.swept_first);
    std::cout << "only local commander sweeps passed\n";
}

// A commander is the type its owner's side names, compared without
// case, whatever the FBI commander flag says. For it alone the kill handler
// clears bit 0 of the owner's Player.resource_flags and counts a commander lost and
// killed; without the commander rule the order panel stays as it is.
void commander_is_named_by_the_side() {
    Fixture f;
    auto& world = f.match->state();
    world.player_info[1].side = 1;
    oa::base::text::copy_terminated(world.game.sides[0].commander, "testunit");
    oa::base::text::copy_terminated(world.game.sides[1].commander, "CORCOM");
    auto& named = idle(f, 0, 64, 64);
    auto& flagged = idle(f, 1, 160, 64);
    auto& killer = idle(f, 1, 200, 200);
    f.run(1);
    PanelCloses closes;
    watch_panel(f, closes);
    CHECK(sim::match_runtime::side_commander(world, named.record));
    f.def.commander = true;
    CHECK(!sim::match_runtime::side_commander(world, flagged.record));
    auto& owner = world.game.players[0];
    auto& enemy = world.game.players[1];
    owner.resource_flags |= 1;
    enemy.resource_flags |= 1;

    sim::match_runtime::MatchTickAccess host(*f.match);
    flagged.record.last_attacker_id = named.unit_index;
    flagged.record.last_attacker_owner = 0;
    flagged.unit->flags |= OA_UNIT_FLAG_DEATH_PENDING;
    host.kill_unit(flagged.record, weapon_hit);
    f.def.commander = false;
    CHECK(!live(flagged) && enemy.losses == 1 && owner.kills == 1);
    CHECK(
        (enemy.resource_flags & 1) != 0 && enemy.commanders_lost == 0 &&
        owner.commanders_killed == 0
    );

    named.record.last_attacker_id = killer.unit_index;
    named.record.last_attacker_owner = 1;
    named.unit->flags |= OA_UNIT_FLAG_DEATH_PENDING;
    host.kill_unit(named.record, weapon_hit);
    CHECK(!live(named) && owner.losses == 1 && enemy.kills == 1);
    CHECK(
        (owner.resource_flags & 1) == 0 && owner.commanders_lost == 1 &&
        enemy.commanders_killed == 1
    );
    CHECK(closes.count == 0);
    std::cout << "commander is named by the side passed\n";
}

// Through the detonation, a first blast starts a shake of half its
// duration (0.5 s is 15 ticks, so 7) and its magnitude on both axes; a blast
// during a shake averages the durations ((7 + 15) / 2 = 11) and adds the
// magnitudes. Once the camera has let the shake go the magnitudes restart
// from zero, and a duration averaged down to zero does not start a shake.
// The console's noshake option and a shot a nosealeveltrigger sea swallows
// add nothing.
void detonation_shakes_the_screen() {
    Fixture f({.high_ground_from = 8});
    arm_death_blasts(f);
    const auto* registered = f.weapons.find("TESTBLAST");
    CHECK(registered->shake_magnitude == 6 && registered->shake_duration_ticks == 15);
    const auto& blast = canonical_weapon(f, "TESTBLAST");
    const auto& quiet = canonical_weapon(f, "TESTSELFD");
    CHECK(blast.shake_magnitude == 6 && blast.shake_duration == 15);
    CHECK(quiet.shake_magnitude == 0 && quiet.shake_duration == 0);
    auto& game = f.match->state().game;
    CHECK(game.shake_duration == 0 && (game.camera_flags & camera_shaking) == 0);
    const std::array<uint32_t, 3> land{160u << 16, 60u << 16, 64u << 16};
    const std::array<uint32_t, 3> sea{64u << 16, 0, 64u << 16};

    CHECK(f.match->spawn_weapon_explosion(blast, land, false));
    CHECK(game.shake_duration == 7 && game.shake_remaining == 7);
    CHECK(game.shake_amplitude_x == 6 && game.shake_amplitude_y == 6);
    CHECK((game.camera_flags & camera_shaking) != 0);
    CHECK(f.match->spawn_weapon_explosion(blast, sea, false));
    CHECK(game.shake_duration == 11 && game.shake_remaining == 11);
    CHECK(game.shake_amplitude_x == 12 && game.shake_amplitude_y == 12);

    game.camera_flags &= static_cast<uint8_t>(~camera_shaking);
    game.shake_remaining = 0;
    CHECK(f.match->spawn_weapon_explosion(blast, land, true));
    CHECK(game.shake_duration == 13 && game.shake_remaining == 13 && game.shake_amplitude_x == 6);

    game.camera_flags &= static_cast<uint8_t>(~camera_shaking);
    game.shake_duration = 1;
    CHECK(f.match->spawn_weapon_explosion(quiet, land, false));
    CHECK(
        game.shake_duration == 0 && game.shake_amplitude_x == 0 &&
        (game.camera_flags & camera_shaking) == 0
    );

    game.console_flags = OA_CONSOLE_FLAG_NO_SHAKE;
    CHECK(f.match->spawn_weapon_explosion(blast, land, false));
    CHECK(game.shake_duration == 0 && game.shake_amplitude_x == 0);
    game.console_flags = 0;
    f.match->effects().no_sea_level_trigger = true;
    CHECK(!f.match->spawn_weapon_explosion(blast, sea, false));
    CHECK(game.shake_duration == 0 && game.shake_amplitude_x == 0);
    f.match->effects().no_sea_level_trigger = false;
    std::cout << "detonation shakes the screen passed\n";
}

// The detonation plays the weapon's soundhit where it goes off on land or on
// a unit and its soundwater where it goes off in the water, through
// play_sound_at:
// only on a map cell (the 16-unit cell of x and z) whose point the viewpoint
// player sees, which lifts z by half of y. A weapon without the sound plays
// nothing.
void detonation_sounds_where_the_viewer_sees() {
    // Each unit sees the sight cells around its own.
    Fixture f({.high_ground_from = 8, .sight_cells = 3});
    arm_death_blasts(f);
    HeardClips heard;
    listen(f, heard);
    auto& shore = idle(f, 0, 160, 224);
    auto& swimmer = idle(f, 0, 64, 64);
    auto& enemy = idle(f, 1, 224, 64);
    f.run(1);
    const auto on_land = sim::match_runtime::fixed_words(shore.record.position);
    const auto at_sea = sim::match_runtime::fixed_words(swimmer.record.position);
    const auto unseen = sim::match_runtime::fixed_words(enemy.record.position);
    // 40 further south and 80 higher: the same sight cell, off the map.
    constexpr uint32_t south = 40u << 16;
    const std::array<uint32_t, 3> beyond{on_land[0], on_land[1] + 2 * south, on_land[2] + south};
    CHECK((beyond[2] >> 20) >= 16);
    CHECK(f.match->point_visible(0, on_land) && f.match->point_visible(0, at_sea));
    CHECK(f.match->point_visible(0, beyond) && !f.match->point_visible(0, unseen));
    const auto& blast = canonical_weapon(f, "TESTBLAST");

    CHECK(f.match->spawn_weapon_explosion(blast, on_land, false));
    CHECK(f.match->spawn_weapon_explosion(blast, at_sea, false));
    CHECK(f.match->spawn_weapon_explosion(blast, at_sea, true));
    CHECK(heard.clips.size() == 3);
    CHECK(
        heard.clips[0].first == "xplomed2" &&
        same_point(heard.clips[0].second, shore.record.position)
    );
    CHECK(
        heard.clips[1].first == "splshbig" &&
        same_point(heard.clips[1].second, swimmer.record.position)
    );
    CHECK(heard.clips[2].first == "xplomed2");
    CHECK(f.match->spawn_weapon_explosion(blast, unseen, false));
    CHECK(f.match->spawn_weapon_explosion(blast, beyond, false));
    CHECK(f.match->spawn_weapon_explosion(canonical_weapon(f, "TESTSELFD"), on_land, false));
    CHECK(heard.clips.size() == 3);
    std::cout << "detonation sounds where the viewer sees passed\n";
}

// Placing a shot ends by playing the weapon's soundstart at the muzzle
// through play_sound_at, so a shot is heard only where the viewpoint player
// sees it
// fired: each of player 0's shots once, none of player 1's.
void weapon_start_sounds_at_the_muzzle() {
    Fixture f({.sight_cells = 3});
    const_cast<sim::combat_state::WeaponDefinition&>(f.weapons.definition(1)).soundstart =
        "cannon1";
    HeardClips heard;
    listen(f, heard);
    auto& seen = f.spawn(0, 64, 64);
    auto& unseen = f.spawn(1, 160, 64);
    f.run(1);
    CHECK(f.match->point_visible(0, sim::match_runtime::fixed_words(seen.record.position)));
    CHECK(!f.match->point_visible(0, sim::match_runtime::fixed_words(unseen.record.position)));
    f.match->apply_damage_event(seen, &unseen, 1, weapon_hit, 0);
    f.match->apply_damage_event(unseen, &seen, 1, weapon_hit, 0);
    f.run(30);
    CHECK(f.shots_from(seen) > 0 && f.shots_from(unseen) > 0);
    CHECK(static_cast<int32_t>(heard.clips.size()) == f.shots_from(seen));
    for (const auto& [name, at] : heard.clips) {
        CHECK(name == "cannon1");
        CHECK(
            at.x >> 20 == seen.record.position.x >> 20 && at.z >> 20 == seen.record.position.z >> 20
        );
    }
    std::cout << "weapon start sounds at the muzzle passed\n";
}

// play_sound_at plays a clip unplaced at -585 while the point lies inside the
// view (camera to camera + view cells * 16, edges included) and at -1585
// outside it. With the sound system's 3D switch on it always plays at -585,
// placed from the middle of the view: x = px - camera x - (cells wide / 2) *
// 16, y = 0, z = (py >> 1) - pz + (cells high / 2) * 16 + camera y, within
// (cells wide + cells high) / 2 * 16 and (map cells wide + high) * 16.
void point_sounds_are_placed_by_the_view() {
    Fixture f({.high_ground_from = 8, .sight_cells = 3});

    struct Heard {
        bool spatial{};
        std::vector<sim::match_runtime::Match::PointSound> sounds;
    } heard;

    f.match->point_sound = {
        &heard,
        [](void* context) { return static_cast<Heard*>(context)->spatial; },
        [](void* context, const char*, const sim::match_runtime::Match::PointSound& sound) {
            static_cast<Heard*>(context)->sounds.push_back(sound);
        }
    };
    auto& shore = idle(f, 0, 160, 224);
    f.run(1);
    const FixedVec3 at = shore.record.position;
    const int32_t px = at.x >> 16;
    const int32_t py = at.y >> 16;
    const int32_t pz = at.z >> 16;
    auto& game = f.match->state().game;
    game.view_cells_width = 10;
    game.view_cells_height = 7;
    game.camera_x = static_cast<uint32_t>(px - 160);
    game.camera_y = static_cast<uint32_t>(pz - 112);
    f.match->play_sound_at("xplomed2", at);
    game.camera_x = static_cast<uint32_t>(px + 1);
    f.match->play_sound_at("xplomed2", at);
    game.camera_x = static_cast<uint32_t>(px - 170);
    heard.spatial = true;
    f.match->play_sound_at("xplomed2", at);
    CHECK(heard.sounds.size() == 3);
    const auto& inside = heard.sounds[0];
    CHECK(!inside.placed && inside.volume == -585 && same_point(inside.at, at));
    const auto& outside = heard.sounds[1];
    CHECK(!outside.placed && outside.volume == -1585);
    const auto& placed = heard.sounds[2];
    CHECK(placed.placed && placed.volume == -585);
    CHECK(placed.x == 170 - 80 && placed.y == 0);
    CHECK(placed.z == (py >> 1) - pz + 48 + (pz - 112));
    CHECK(placed.min_distance == 128.0F);
    CHECK(placed.max_distance == static_cast<float>((game.map_width + game.map_height) * 16));
    std::cout << "point sounds are placed by the view passed\n";
}

// The kill handler kills a transport's cargo through the damage path from the transport's
// killer with 30000 of kind 6, or of kind 3 when the transport
// self-destructed, so a veteran passenger takes less.
void cargo_dies_through_scaled_damage() {
    Fixture f;
    auto& carrier = idle(f, 0, 64, 64);
    auto& cargo = idle(f, 0, 80, 64);
    auto& ferry = idle(f, 0, 64, 160);
    auto& crew = idle(f, 0, 80, 160);
    auto& killer = idle(f, 1, 200, 200);
    f.run(1);
    cargo.record.veteran_level = 12;
    crew.record.veteran_level = 30;
    f.match->set_carry_link(cargo.unit_index, carrier.unit_index, -1, 1);
    f.match->set_carry_link(crew.unit_index, ferry.unit_index, -1, 1);
    CHECK(cargo.record.attach_parent != 0 && crew.record.attach_parent != 0);

    kill(f, carrier, killer);
    CHECK(cargo.record.attach_parent == 0 && dying(cargo));
    CHECK(cargo.unit->health == after_hit(30000, 12) && cargo.unit->health == -26600);
    CHECK(cargo.record.damage_kind == cargo_death);
    CHECK(
        cargo.record.last_attacker_id == killer.unit_index && cargo.record.last_attacker_owner == 1
    );

    ferry.unit->record.damage_kind = self_destruct;
    ferry.record.last_attacker_id = ferry.unit_index;
    ferry.record.last_attacker_owner = 0;
    f.match->teardown_dead_unit(ferry);
    CHECK(crew.record.attach_parent == 0 && dying(crew));
    CHECK(crew.unit->health == after_hit(30000, 30) && crew.unit->health == -23000);
    CHECK(
        crew.record.damage_kind == self_destruct && crew.record.last_attacker_id == ferry.unit_index
    );
    std::cout << "cargo dies through scaled damage passed\n";
}

// A capture kills the unit it copies for the capturer through the damage path with
// 30000 of kind 4 from no source; so does a capture by a mirrored player's
// unit, which leaves no copy here.
void capture_kills_through_scaled_damage() {
    Fixture f;
    auto& capturer = idle(f, 0, 64, 64);
    auto& taken = idle(f, 1, 160, 64);
    auto& lost = idle(f, 1, 160, 160);
    f.run(1);
    auto& captor = f.match->state().game.players[0];
    const auto units = captor.unit_count;
    taken.record.veteran_level = 25;
    f.match->capture_unit(taken, capturer);
    CHECK(
        dying(taken) && taken.unit->health == after_hit(30000, 25) && taken.unit->health == -23000
    );
    CHECK(taken.record.damage_kind == captured && taken.record.last_attacker_id == 0);
    CHECK(captor.unit_count == units + 1);

    lost.record.veteran_level = 7;
    f.match->simulation().players[0].status = OA_PLAYER_STATUS_MIRRORED;
    f.match->capture_unit(lost, capturer);
    f.match->simulation().players[0].status = OA_PLAYER_STATUS_LOCAL;
    CHECK(dying(lost) && lost.unit->health == after_hit(30000, 7) && lost.unit->health == -27800);
    CHECK(lost.record.damage_kind == captured && captor.unit_count == units + 1);
    std::cout << "capture kills through scaled damage passed\n";
}

// A capture, a hand-over another machine started, and a transfer that changes
// nothing: the listener hears the hand-over after the copy is created and
// before the old unit dies, and hears nothing when no unit changes hands.
void hand_overs_are_reported() {
    enum class Kind : uint8_t { created, created_by, given, died };

    struct Entry {
        Kind kind{};
        uint16_t unit{};
        uint16_t other{};
        uint8_t from{};
        uint8_t to{};
        bool live{};
    };

    struct Log {
        std::vector<Entry> entries;
    } log;

    Fixture f;
    auto& capturer = idle(f, 0, 64, 64);
    auto& taken = idle(f, 1, 160, 64);
    auto& lost = idle(f, 1, 160, 160);
    auto& spare = idle(f, 1, 64, 160);
    auto& carried_unit = idle(f, 1, 200, 200);
    f.run(1);

    sim::match_runtime::EventHooks hooks;
    hooks.context = &log;
    hooks.unit_created = [](void* context, const World& world, uint16_t unit) {
        static_cast<Log*>(context)->entries.push_back(
            {Kind::created, unit, 0, 0, 0, (world.units[unit].flags & OA_UNIT_FLAG_LIVE) != 0}
        );
    };
    hooks.unit_created_by = [](void* context, const World&, uint16_t unit, uint16_t builder) {
        static_cast<Log*>(context)->entries.push_back({Kind::created_by, unit, builder});
    };
    hooks.unit_given = [](void* context,
                          const World& world,
                          uint16_t unit,
                          uint16_t copy,
                          uint8_t from,
                          uint8_t to) {
        static_cast<Log*>(context)->entries.push_back(
            {Kind::given, unit, copy, from, to, (world.units[unit].flags & OA_UNIT_FLAG_LIVE) != 0}
        );
    };
    hooks.unit_died =
        [](
            void* context, const World&, uint16_t unit, const sim::match_runtime::KillOutcome&, bool
        ) { static_cast<Log*>(context)->entries.push_back({Kind::died, unit}); };
    CHECK(f.match->add_event_listener(hooks) != 0);

    // A transfer that changes nothing reports none of the four events.
    CHECK(!f.match->simulation().players[4].present);
    f.match->transfer_unit(spare.unit_index, spare.record.owner_index);
    spare.unit->flags |= OA_UNIT_FLAG_DEATH_PENDING;
    f.match->transfer_unit(spare.unit_index, 0);
    spare.unit->flags &= ~OA_UNIT_FLAG_DEATH_PENDING;
    f.match->transfer_unit(spare.unit_index, 4);
    CHECK(log.entries.empty());
    CHECK(live(spare) && !dying(spare) && spare.record.owner_index == 1);

    f.match->capture_unit(taken, capturer);
    CHECK(log.entries.size() == 3);
    CHECK(log.entries[0].kind == Kind::created);
    CHECK(log.entries[1].kind == Kind::created_by && log.entries[1].unit == log.entries[0].unit);
    CHECK(log.entries[1].other == 0);
    CHECK(
        log.entries[2].kind == Kind::given && log.entries[2].unit == taken.unit_index &&
        log.entries[2].other == log.entries[0].unit && log.entries[2].from == 1 &&
        log.entries[2].to == 0 && log.entries[2].live
    );
    const auto copy = log.entries[0].unit;
    const auto& copied = f.match->state().units[copy];
    CHECK((copied.flags & OA_UNIT_FLAG_LIVE) != 0 && copied.owner_index == 0);
    CHECK(dying(taken));

    f.run(1);
    CHECK(!live(taken));
    bool taken_died = false;
    for (size_t index = 3; index < log.entries.size(); ++index) {
        CHECK(log.entries[index].kind != Kind::created);
        if (log.entries[index].kind == Kind::died && log.entries[index].unit == taken.unit_index)
            taken_died = true;
    }
    CHECK(taken_died);
    CHECK((f.match->state().units[copy].flags & OA_UNIT_FLAG_LIVE) != 0);
    CHECK(f.match->state().units[copy].owner_index == 0);

    log.entries.clear();
    f.match->simulation().players[0].status = OA_PLAYER_STATUS_MIRRORED;
    f.match->capture_unit(lost, capturer);
    f.match->simulation().players[0].status = OA_PLAYER_STATUS_LOCAL;
    CHECK(log.entries.size() == 1);
    CHECK(
        log.entries[0].kind == Kind::given && log.entries[0].unit == lost.unit_index &&
        log.entries[0].other == 0 && log.entries[0].from == 1 && log.entries[0].to == 0 &&
        log.entries[0].live
    );
    f.run(1);
    CHECK(!live(lost));
    bool lost_died = false;
    for (const auto& entry : log.entries) {
        CHECK(entry.kind != Kind::created && entry.kind != Kind::created_by);
        if (entry.kind == Kind::died && entry.unit == lost.unit_index)
            lost_died = true;
    }
    CHECK(lost_died);

    log.entries.clear();
    sim::match_runtime::TransferredUnit carried;
    carried.health = 40;
    f.match->transfer_unit(carried_unit.unit_index, 0, &carried);
    CHECK(log.entries.size() == 3);
    CHECK(log.entries[0].kind == Kind::created);
    CHECK(log.entries[1].kind == Kind::created_by && log.entries[1].unit == log.entries[0].unit);
    CHECK(log.entries[1].other == 0);
    CHECK(
        log.entries[2].kind == Kind::given && log.entries[2].unit == carried_unit.unit_index &&
        log.entries[2].other == log.entries[0].unit && log.entries[2].from == 1 &&
        log.entries[2].to == 0 && log.entries[2].live
    );
    CHECK(live(carried_unit) && !dying(carried_unit));
    CHECK(f.match->state().units[log.entries[0].unit].owner_index == 0);
    f.run(2);
    CHECK(live(carried_unit) && !dying(carried_unit));
    for (const auto& entry : log.entries)
        CHECK(entry.kind != Kind::died || entry.unit != carried_unit.unit_index);
    std::cout << "hand overs are reported passed\n";
}

// The hotkey self-destruct gives each unit a SelfDestruct order, which deals
// 30000 of kind 3 from the unit itself through the damage path: on its
// first step under a zero countdown, else after one count a second and a
// last wait of under 15 ticks.
void hotkey_self_destruct_is_scaled() {
    Fixture f;
    auto& sudden = idle(f, 0, 64, 64);
    auto& counted = idle(f, 0, 160, 64);
    f.run(1);
    sudden.record.veteran_level = 20;
    auto& def = f.match->state().unit_defs[1];
    const auto countdown = [&def](uint32_t counts) {
        def.abilities = (def.abilities & ~OA_UNIT_DEF_ABILITY_SELF_DESTRUCT_MASK) |
                        (counts << OA_UNIT_DEF_ABILITY_SELF_DESTRUCT_SHIFT);
    };
    countdown(0);
    const std::array<uint16_t, 1> first{sudden.unit_index};
    f.match->toggle_self_destruct(first);
    auto* order = f.match->orders(sudden.unit_index).secondary;
    CHECK(order && order->kind == sim::match_runtime::self_destruct_kind);
    // Stepped outside the tick, which would also run the kill handler.
    sim::match_runtime::MatchTickAccess host(*f.match);
    CHECK(host.dispatch_mission(f.match->state(), sudden.record, *order, 0) == 5);
    CHECK(
        dying(sudden) && sudden.unit->health == after_hit(30000, 20) &&
        sudden.unit->health == -24200
    );
    CHECK(
        sudden.record.damage_kind == self_destruct &&
        sudden.record.last_attacker_id == sudden.unit_index
    );
    f.match->stop_orders(sudden.unit_index);

    countdown(1);
    const std::array<uint16_t, 1> second{counted.unit_index};
    f.match->toggle_self_destruct(second);
    CHECK(f.match->self_destruct_remaining(counted.unit_index) == 1);
    f.run(30);
    CHECK(live(counted) && counted.unit->health == full_health);
    uint32_t ticks = 30;
    while (live(counted) && ticks < 30 + 16) {
        f.run(1);
        ++ticks;
    }
    CHECK(!live(counted));
    CHECK(
        counted.record.damage_kind == self_destruct &&
        counted.record.last_attacker_id == counted.unit_index
    );
    std::cout << "hotkey self-destruct is scaled passed\n";
}

// The kill handler tells the corpse placement whether the death was other
// than kind 7: a
// dismissed unit leaves its corpse without the 900-tick smoke column a
// weapon death raises on dry ground.
void dismissed_corpse_does_not_smoke() {
    FeatureDef corpse{};
    corpse.footprint_x = corpse.footprint_z = 1;
    corpse.dead_feature = 0xffff;
    // The smoke column draws FX.GAF's "smoke 1".
    const auto smoke_art = BlastArt::frames(4);
    Fixture f(
        {.high_ground_from = 0,
         .effect_sequence = [&smoke_art](
                                std::string_view, std::string_view entry
                            ) { return entry == "smoke 1" ? &smoke_art : nullptr; },
         .features = {corpse}}
    );
    auto& dismissed = idle(f, 0, 64, 64);
    // Above the bottom band the map load hides, where no corpse fits.
    auto& shot = idle(f, 0, 160, 96);
    f.run(1);
    auto& world = f.match->state();
    oa::world_unit_def_of(&world, &dismissed.record)->corpse = 0;
    const auto& smoke = f.match->effects().layers[sim::effect_particles::layer_smoke];
    const auto columns = smoke.count;
    const auto wrecks = f.match->wrecks().size();
    f.match->teardown_dead_unit(dismissed, {sim::match_runtime::DeathKind::dismissed, 0, 1});
    CHECK(f.match->wrecks().size() == wrecks + 1 && smoke.count == columns);
    f.match->teardown_dead_unit(shot, {sim::match_runtime::DeathKind::weapon, 0, 1});
    CHECK(f.match->wrecks().size() == wrecks + 2 && smoke.count == columns + 1);
    std::cout << "dismissed corpse does not smoke passed\n";
}

// The SelfDestruct order damages the unit from itself.
void self_destruct_order_is_its_own_source() {
    Fixture f;
    auto& unit = idle(f, 0, 64, 64);
    f.run(1);
    (void)f.match->insert_ground_order(unit.unit_index, sim::match_runtime::self_destruct_kind);
    f.run(1);
    CHECK(!live(unit));
    CHECK(unit.record.last_attacker_id == unit.unit_index && unit.record.last_attacker_owner == 0);
    CHECK(f.match->state().game.players[0].losses == 1);
    std::cout << "self-destruct order is its own source passed\n";
}

// The fixture scenario's UnitTypeKilled condition.
const oa::sim::scenario::Condition& unit_type_killed_condition(Fixture& f) {
    const auto& defeat = f.match->scenario_controller().defeat;
    const auto found = std::find_if(defeat.begin(), defeat.end(), [](const auto& c) {
        return c && c->kind == oa::sim::scenario::Kind::unit_type_killed;
    });
    CHECK(found != defeat.end());
    return **found;
}

// UnitTypeKilled counts down every death of its type, whoever owns the unit,
// and the destroyed broadcast goes on calling it
// once the condition is met.
void unit_type_killed_counts_every_death() {
    Fixture f({.unit_type_killed = "testunit,2"});
    auto& own = idle(f, 0, 64, 64);
    auto& enemy = idle(f, 1, 160, 64);
    auto& spare = idle(f, 0, 64, 160);
    auto& killer = idle(f, 1, 160, 160);
    f.run(1);
    const auto& condition = unit_type_killed_condition(f);
    kill(f, own, killer);
    CHECK(condition.kills_left == 1 && condition.satisfied == 0);
    kill(f, enemy, killer);
    CHECK(condition.kills_left == 0 && condition.satisfied == 1);
    kill(f, spare, killer);
    CHECK(condition.kills_left == -1 && condition.satisfied == 1);
    std::cout << "unit type killed counts every death passed\n";
}

// A unit that self-destructs inside a tick reaches UnitTypeKilled from the death
// step: a unit of another type leaves the kills left alone, one of the named
// type meets the condition, and the match goes on ticking after both.
void unit_type_killed_counts_tick_deaths() {
    for (const bool named : {false, true}) {
        Fixture f({.unit_type_killed = named ? "TestUnit,1" : "ARMMOHO,1"});
        auto& unit = idle(f, 1, 64, 64);
        auto& bystander = idle(f, 0, 160, 160);
        f.run(1);
        const auto& condition = unit_type_killed_condition(f);
        (void)f.match->insert_ground_order(unit.unit_index, sim::match_runtime::self_destruct_kind);
        f.run(1);
        CHECK(!live(unit));
        CHECK(condition.kills_left == (named ? 0 : 1));
        CHECK(condition.satisfied == (named ? 1 : 0));
        const uint32_t tick = f.match->simulation().tick;
        f.run(30);
        CHECK(f.match->simulation().tick == tick + 30 && live(bystander));
        CHECK(condition.kills_left == (named ? 0 : 1));
    }
    std::cout << "unit type killed counts tick deaths passed\n";
}

// A unit made in a dead unit's slot gets its instance under the next
// generation, wherever the new instance lands in memory, and each later
// spawn in the slot advances it again; the other slots keep theirs.
void spawn_in_dead_slot_advances_instance_generation() {
    constexpr uint32_t first_generation = 1;
    constexpr uint32_t respawns = 3;
    Fixture f;
    auto& unit = idle(f, 0, 64, 64);
    auto& bystander = idle(f, 1, 160, 160);
    const auto slot = unit.unit_index;
    CHECK(f.match->runtime_state(slot).instance_generation == first_generation);
    CHECK(f.match->runtime_state(bystander.unit_index).instance_generation == first_generation);
    for (uint32_t respawn = 1; respawn <= respawns; ++respawn) {
        f.run(1);
        f.match->teardown_dead_unit(unit, {sim::match_runtime::DeathKind::dismissed, 0, 0});
        CHECK(!live(unit) && f.match->instance(slot) == nullptr);
        CHECK(f.match->runtime_state(slot).instance_generation == first_generation + respawn - 1);
        auto& next = idle(f, 0, 64, 64);
        CHECK(next.unit_index == slot && f.match->instance(slot) != nullptr);
        CHECK(f.match->runtime_state(slot).instance_generation == first_generation + respawn);
    }
    CHECK(f.match->runtime_state(bystander.unit_index).instance_generation == first_generation);
    std::cout << "spawn in dead slot advances instance generation passed\n";
}

// A unit of a type with no maximum health (MaxDamage 0) is made with no health
// and reads 0% at each 30-tick reading, with no fault, and the units after it
// in the tick's pass are still updated. Its first hit kills it through the
// kill handler, the health quotient taken as 0: Killed runs at half the
// reading before, the unit explodes with its explodeas weapon and leaves the
// wreck Killed picks, and it is torn down.
void unit_without_maximum_health_dies_on_first_hit() {
    struct Death {
        uint16_t unit{};
        sim::match_runtime::KillOutcome outcome{};
    };

    const BlastArt art;
    auto options = art.options();
    options.maximum_health = 0;
    options.killed_script = true;
    Fixture f(options);
    arm_death_blasts(f);
    std::vector<Death> deaths;
    f.match->event_hooks.context = &deaths;
    f.match->event_hooks.unit_died = [](void* context,
                                        const World&,
                                        uint16_t unit,
                                        const sim::match_runtime::KillOutcome& outcome,
                                        bool) {
        static_cast<std::vector<Death>*>(context)->push_back({unit, outcome});
    };
    auto& victim = idle(f, 0, 64, 64);
    auto& later = idle(f, 1, 200, 200);
    CHECK(victim.unit->health == 0 && live(victim) && !dying(victim));
    while (f.match->simulation().tick % 30 != 29)
        f.run(1);
    // The reading before, which the Killed percentage halves.
    victim.record.health_percent = 80;
    later.record.health_percent = 70;
    later.record.damage_countdown = 10;
    f.run(1);
    CHECK(victim.record.previous_health_percent == 80 && victim.record.health_percent == 0);
    CHECK(later.record.previous_health_percent == 70 && later.record.health_percent == 0);
    CHECK(later.record.damage_countdown == 9);
    CHECK(live(victim) && !dying(victim));

    const auto& effects = std::as_const(*f.match).effects();
    const auto blasts = effects.explosion_count;
    f.match->apply_damage_event(victim, &later, 1, weapon_hit, 0);
    CHECK(dying(victim));
    f.run(1);
    CHECK(!live(victim));
    CHECK(deaths.size() == 1 && deaths[0].unit == victim.unit_index);
    CHECK(deaths[0].outcome.killed_percent == 40);
    CHECK(deaths[0].outcome.wreck_level == killed_corpsetype(40));
    CHECK(f.services.explosions.size() == 1);
    CHECK(f.services.explosions[0].unit == victim.unit_index);
    CHECK(f.services.explosions[0].flags == killed_corpsetype(40));
    CHECK(effects.explosion_count == blasts + 1);
    CHECK(effects.explosions[blasts].sprite.sequence == &art.explode4);
    CHECK(live(later));
    f.run(30);
    std::cout << "unit without maximum health dies on first hit passed\n";
}

} // namespace

int main() {
    try {
        death_blast_damages_neighbours();
        sweep_damages_local_units();
        sweep_of_empty_player_returns();
        sweep_kills_mirrored_units();
        only_local_commander_sweeps();
        commander_is_named_by_the_side();
        self_destruct_order_is_its_own_source();
        detonation_shakes_the_screen();
        detonation_sounds_where_the_viewer_sees();
        point_sounds_are_placed_by_the_view();
        weapon_start_sounds_at_the_muzzle();
        cargo_dies_through_scaled_damage();
        capture_kills_through_scaled_damage();
        hand_overs_are_reported();
        hotkey_self_destruct_is_scaled();
        dismissed_corpse_does_not_smoke();
        unit_type_killed_counts_every_death();
        unit_type_killed_counts_tick_deaths();
        spawn_in_dead_slot_advances_instance_generation();
        unit_without_maximum_health_dies_on_first_hit();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
