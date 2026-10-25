// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Map features in a running match: reclaim sequences, fire from a weapon
// spreading by the feature pass, die sequences leaving featuredead and a
// geothermal vent's smoke placed with the map; the mission schema's
// placements after the map's own and the metal the features paint, the
// header's water damage, the meteor storm's step and launch, and a small
// blast at a wreck's edge.
#include "combat_fixture.hpp"
#include "oa/sim/weapon_execution/projectile_pool.hpp"
#include "oa/sim/weapon_execution/weapon_launch.hpp"

#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>
#include <string_view>
#include <string>
#include <cstdint>

namespace {

using namespace combat_fixture;
namespace features = oa::sim::feature_runtime;

constexpr uint16_t tree = 0, rock = 1, crisp = 2, smudge = 3, vent = 4, wall = 5, wreck = 6;
constexpr int32_t map_cells = 16;
constexpr uint8_t registry_gun = 1; // TESTGUN
constexpr int32_t high_ground = 60; // plot height Options::high_ground_from raises

// Sequence refs, as the app's FeatureDefHost would number them.
constexpr oa_ref32 standing = 1, burning = 2, dying = 3, reclaiming = 4, venting = 5;

struct Shape {
    uint16_t frames{};
    uint16_t duration{};
    uint8_t repeat{};
};

// Frame counts and per-frame ticks of each sequence; the burn, die and
// reclamate sequences are one-shot (the loader cleared their repeat byte).
// The burn is as long as ArchiTree01's: 98 frames of two ticks.
constexpr Shape shapes[] = {{}, {1, 1, 1}, {98, 2, 0}, {3, 4, 0}, {4, 3, 0}, {2, 5, 1}};

bool sequence_frame(oa_ref32 sequence, uint16_t frame, features::FeatureSequenceFrame& out) {
    if (sequence == 0 || sequence >= std::size(shapes))
        return false;
    const auto& shape = shapes[sequence];
    out.frame_count = shape.frames;
    out.repeat = shape.repeat;
    if (frame >= shape.frames)
        return false;
    out.duration = shape.duration;
    out.width = 16;
    out.height = 32;
    out.origin_x = 8;
    out.origin_y = 30;
    return true;
}

void set_links(FeatureDef& def, uint16_t dead, uint16_t burnt, uint16_t reclamate) {
    def.dead_feature = dead;
    def.burnt_feature = burnt;
    def.reclamate_feature = reclamate;
}

std::vector<FeatureDef> feature_table() {
    std::vector<FeatureDef> table(7);
    for (auto& def : table) {
        def.footprint_x = def.footprint_z = 1;
        def.flags = OA_FEATURE_FLAG_SPRITE;
        def.seq_name = standing;
        set_links(def, features::no_feature, features::no_feature, features::no_feature);
    }
    // A tree like ArchiTree01: sparktime 5, which loads as 150 ticks, burns
    // into its crisp remnant, dies into a smudge and is reclaimed to nothing.
    auto& t = table[tree];
    t.flags |= OA_FEATURE_FLAG_FLAMABLE | OA_FEATURE_FLAG_RECLAIMABLE | OA_FEATURE_FLAG_BLOCKING;
    t.seq_name_burn = burning;
    t.seq_name_die = dying;
    t.seq_name_reclamate = reclaiming;
    t.spark_time = 150;
    t.spread_chance = 100;
    t.damage = 50;
    t.energy = 250.0F;
    t.burn_weapon = oa::oa_ref_from_index(registry_gun);
    set_links(t, smudge, crisp, features::no_feature);
    auto& r = table[rock];
    r.flags |= OA_FEATURE_FLAG_RECLAIMABLE | OA_FEATURE_FLAG_BLOCKING;
    r.seq_name_reclamate = reclaiming;
    r.metal = 30.0F;
    auto& v = table[vent];
    v.flags |=
        OA_FEATURE_FLAG_GEOTHERMAL | OA_FEATURE_FLAG_INDESTRUCTIBLE | OA_FEATURE_FLAG_ANIMATING;
    v.seq_name = venting;
    // A two-cell 3DO wall like DragonsTeeth; placements centre it.
    auto& w = table[wall];
    w.flags = OA_FEATURE_FLAG_BLOCKING;
    w.footprint_x = w.footprint_z = 2;
    // A 3DO unit wreck like armfast_dead.
    auto& d = table[wreck];
    d.flags = OA_FEATURE_FLAG_BLOCKING | OA_FEATURE_FLAG_RECLAIMABLE;
    d.footprint_x = d.footprint_z = 2;
    d.height = 20;
    d.damage = 440;
    const char* names[] = {"Tree", "Rock", "Crisp", "Smudge", "Vent", "Wall", "Wreck"};
    for (std::size_t index = 0; index < table.size(); ++index)
        std::snprintf(table[index].name, sizeof table[index].name, "%s", names[index]);
    return table;
}

std::size_t plot(int32_t x, int32_t z) {
    return static_cast<std::size_t>(z * map_cells + x);
}

formats::gaf::Sequence smoke_art = [] {
    formats::gaf::Sequence sequence{};
    sequence.frames.resize(8);
    for (auto& frame : sequence.frames)
        frame.duration = 4;
    return sequence;
}();

// Trees at (8,8) and (9,8), a far tree at (3,12), a rock at (4,4) and a vent
// at (12,3), all placed by the loader; weapons damage features.
Options feature_options() {
    Options options;
    options.features = feature_table();
    options.feature_words = {
        {plot(8, 8), tree},
        {plot(9, 8), tree},
        {plot(3, 12), tree},
        {plot(4, 4), rock},
        {plot(12, 3), vent}
    };
    options.feature_sequence_frame = sequence_frame;
    options.effect_sequence = [](std::string_view,
                                 std::string_view entry) -> const formats::gaf::Sequence* {
        return entry == "smoke 1" || entry == "smoke 2" ? &smoke_art : nullptr;
    };
    return options;
}

void allow_feature_damage(Fixture& f) {
    f.match->state().game.console_flags |= OA_CONSOLE_FLAG_TREE_DEATH;
}

void arm_gun(Fixture& f, uint16_t area_of_effect, uint16_t damage, bool fire) {
    const auto keys = "areaofeffect=" + std::to_string(area_of_effect) +
                      "; firestarter=" + (fire ? "1" : "0") + ";";
    CHECK(
        sim::combat_state::install_weapon_text(
            f.weapons, combat_fixture::test_gun_tdf("0.1", keys, damage)
        ) == 1
    );
    std::copy(
        f.weapons.records().begin(),
        f.weapons.records().end(),
        std::begin(f.match->state().game.weapon_defs)
    );
}

// A blast of TESTGUN at a point, with no firing unit.
void blast_at(Fixture& f, const FixedVec3& point) {
    oa::Projectile shot{};
    shot.def = oa::oa_ref_from_index(registry_gun);
    shot.position = point;
    shot.owner_index = features::no_player;
    f.match->detonate(shot, nullptr);
}

// A 32-AoE blast of TESTGUN centred on a cell.
void blast(Fixture& f, int32_t x, int32_t z, uint16_t damage, bool fire) {
    arm_gun(f, 32, damage, fire);
    blast_at(f, {(x * 16 + 8) << 16, 0, (z * 16 + 8) << 16});
}

const features::PlacedFeature* record_at(Fixture& f, int32_t x, int32_t z) {
    const auto& cell = f.match->state().plots[plot(x, z)];
    if ((cell.flags & OA_PLOT_FLAG_ANIMATING_FEATURE) == 0)
        return nullptr;
    return features::feature_record(f.match->state(), cell.feature_record);
}

// A reclaimed rock credits its metal, keeps its plot while the four-frame
// reclamate sequence plays three ticks a frame, then leaves nothing.
void reclaimed_rock_plays_its_sequence() {
    Fixture f(feature_options());
    auto& unit = f.spawn(0, 64, 96);
    auto& world = f.match->state();
    const FixedVec3 at{(4 * 16 + 8) << 16, 0, (4 * 16 + 8) << 16};
    const auto metal = unit.record.economy.metal.produced;
    CHECK(f.match->reclaim_feature(unit.record, at));
    CHECK(unit.record.economy.metal.produced == metal + 30.0F);
    const auto* record = record_at(f, 4, 4);
    CHECK(
        record && record->sprite.animation.sequence == reclaiming &&
        (record->state & features::state_reclaimed) != 0
    );
    CHECK(!f.match->reclaim_feature(unit.record, at));
    constexpr uint32_t sequence_ticks = 4 * 3;
    f.run(sequence_ticks - 1);
    CHECK(
        world.plots[plot(4, 4)].feature == rock &&
        f.match->spatial().plots[plot(4, 4)].blocking_feature
    );
    f.run(1);
    CHECK(world.plots[plot(4, 4)].feature == features::no_feature);
    CHECK(f.match->spatial().plots[plot(4, 4)].feature_word == sim::spatial_state::no_feature);
    CHECK(!f.match->spatial().plots[plot(4, 4)].blocking_feature);
    std::cout << "reclaimed rock plays its sequence passed\n";
}

// A fire-starting blast lights the tree: its spread countdown is half the
// spark time plus a shared-stream draw below that half, 75 to 149 ticks. It
// smokes on every third tick, lights the neighbour when the countdown runs
// out (firing its burn weapon there) and burns out into featureburnt after
// its 98-frame, two-tick burn sequence. The far tree never catches.
void burning_tree_spreads() {
    Fixture f(feature_options());
    allow_feature_damage(f);
    // A unit on the ground beside the tree, inside the burn weapon's blast.
    auto& bystander = f.spawn(1, 8 * 16 + 8, 8 * 16 + 8 + 14);
    bystander.record.position.y = 0;
    auto& world = f.match->state();
    sim::match_runtime::SharedRandom probe(0);
    probe.restore(f.match->random_state());
    const auto half = static_cast<uint32_t>(world.feature_defs[tree].spark_time) >> 1;
    const auto countdown = probe.bounded(half) + half;
    const auto unhurt = bystander.unit->health;

    // Catching fire plays "treeburn" through play_named_sound_at, which looks
    // the name up in the sound table and plays that entry's file at the cell.
    struct Heard {
        std::vector<std::string> asked;
        std::vector<std::string> played;
    } heard;

    f.match->named_sound = {&heard, nullptr, [](void* context, const char* name) -> const char* {
                                static_cast<Heard*>(context)->asked.emplace_back(name);
                                return std::string_view(name) == "treeburn" ? "burn02" : nullptr;
                            }};
    f.match->point_sound = {
        &heard,
        nullptr,
        [](void* context, const char* name, const sim::match_runtime::Match::PointSound&) {
            static_cast<Heard*>(context)->played.emplace_back(name);
        }
    };
    // The bystander's owner sees the tree's cell.
    const auto viewpoint = world.game.viewpoint_player;
    world.game.viewpoint_player = bystander.record.owner_index;
    blast(f, 8, 8, 100, true);
    world.game.viewpoint_player = viewpoint;
    CHECK(heard.asked == std::vector<std::string>{"treeburn"});
    CHECK(heard.played == std::vector<std::string>{"burn02"});
    CHECK(bystander.unit->health < unhurt);
    const auto* burning_tree = record_at(f, 8, 8);
    CHECK(burning_tree && (burning_tree->state & features::state_burning) != 0);
    CHECK(burning_tree->spread_countdown == countdown && countdown >= 75 && countdown <= 149);
    CHECK(record_at(f, 9, 8) == nullptr);
    const auto health = bystander.unit->health;
    const auto& smoke = f.match->effects().layers[features::burning_smoke_layer];
    // Puffs are counted while the first ones still rise; later ones replace
    // those that fade.
    constexpr uint32_t counted_smoke_ticks = 4 * features::smoke_period;
    for (uint32_t tick = 1; tick < countdown; ++tick) {
        const auto puffs = smoke.count;
        f.run(1);
        if (tick <= counted_smoke_ticks) {
            CHECK(smoke.count == puffs + (world.game.tick % features::smoke_period == 0 ? 1 : 0));
        } else {
            CHECK(smoke.count != 0);
        }
        CHECK(record_at(f, 9, 8) == nullptr);
    }
    f.run(1);
    const auto* neighbour = record_at(f, 9, 8);
    CHECK(neighbour && (neighbour->state & features::state_burning) != 0);
    CHECK(bystander.unit->health < health);
    CHECK(record_at(f, 3, 12) == nullptr && world.plots[plot(3, 12)].feature == tree);
    constexpr uint32_t burn_ticks = 98 * 2;
    f.run(burn_ticks - countdown - 1);
    CHECK(world.plots[plot(8, 8)].feature == tree);
    f.run(1);
    CHECK(world.plots[plot(8, 8)].feature == crisp);
    std::cout << "burning tree spreads passed\n";
}

// Damage below the tree's hit points stays in its plot's record word; the
// blow that reaches them starts the die sequence (three frames of four
// ticks), after which the featuredead smudge stands in its place.
void destroyed_tree_leaves_its_remnant() {
    Fixture f(feature_options());
    allow_feature_damage(f);
    auto& world = f.match->state();
    blast(f, 3, 12, 30, false);
    CHECK(world.plots[plot(3, 12)].feature_record == 30 && record_at(f, 3, 12) == nullptr);
    blast(f, 3, 12, 30, false);
    const auto* dying_tree = record_at(f, 3, 12);
    CHECK(
        dying_tree && dying_tree->sprite.animation.sequence == dying &&
        (dying_tree->state & features::state_burning) == 0
    );
    constexpr uint32_t die_ticks = 3 * 4;
    f.run(die_ticks - 1);
    CHECK(world.plots[plot(3, 12)].feature == tree);
    f.run(1);
    CHECK(world.plots[plot(3, 12)].feature == smudge && record_at(f, 3, 12) == nullptr);
    std::cout << "destroyed tree leaves its remnant passed\n";
}

// The loader's vent starts endless feature smoke on layer 4 at its cell's
// centre; a puff rises every five ticks.
void vent_smokes_every_five_ticks() {
    Fixture f(feature_options());
    const auto& layer = f.match->effects().layers[features::geothermal_smoke_layer];
    CHECK(layer.count == 1);
    const auto& emitter = sim::effect_particles::layer_emitter(
        f.match->effects(), features::geothermal_smoke_layer, 0
    );
    CHECK(
        emitter.kind == sim::effect_particles::EmitterKind::feature_smoke && emitter.interval == 5
    );
    CHECK(emitter.origin.x == (12 * 16 + 8) << 16 && emitter.origin.z == (3 * 16 + 8) << 16);
    std::vector<uint32_t> puffs;
    auto next = emitter.next_spawn;
    for (uint32_t tick = 0; tick < 20; ++tick) {
        f.run(1);
        if (emitter.next_spawn != next) {
            const uint32_t now = f.match->state().game.tick;
            puffs.push_back(now);
        }
        next = emitter.next_spawn;
    }
    CHECK((puffs == std::vector<uint32_t>{5, 10, 15, 20}));
    std::cout << "vent smokes every five ticks passed\n";
}

// The map load places the schema's features after the map's own: the
// placement finds each by name, centres a 3DO feature on its point by half its
// footprint and puts a sprite's corner there. Entries the parser blanked
// (a negative coordinate) and unknown names are skipped. The hidden edges
// come last: a wall in the bottom band keeps its origin plot
// while its continuation plots become hidden-edge plots.
void mission_features_follow_the_map() {
    auto options = feature_options();
    const auto placement = [](const char* name, int32_t x, int32_t z) {
        features::FeaturePlacement entry{};
        std::snprintf(entry.name, sizeof entry.name, "%s", name);
        entry.x = x;
        entry.z = z;
        return entry;
    };
    options.mission_features = {
        placement("rock", 6, 6),
        placement("WALL", 11, 5),
        placement("", 2, 2),
        placement("Boulder", 1, 1),
        placement("wall", 7, 11)
    };
    Fixture f(options);
    const auto& world = f.match->state();
    const auto& spatial = f.match->spatial().plots;
    CHECK(world.plots[plot(4, 4)].feature == rock);
    CHECK(world.plots[plot(6, 6)].feature == rock && spatial[plot(6, 6)].blocking_feature);
    CHECK(world.plots[plot(10, 4)].feature == wall && record_at(f, 10, 4) != nullptr);
    CHECK(world.plots[plot(11, 5)].feature == features::feature_continuation);
    CHECK(spatial[plot(11, 5)].blocking_feature && spatial[plot(10, 5)].blocking_feature);
    CHECK(
        world.plots[plot(11, 5)].feature != wall &&
        world.plots[plot(12, 6)].feature == features::no_feature
    );
    CHECK(world.plots[plot(2, 2)].feature == features::no_feature);
    CHECK(world.plots[plot(1, 1)].feature == features::no_feature);
    CHECK(world.plots[plot(6, 10)].feature == wall && record_at(f, 6, 10) != nullptr);
    CHECK(
        world.plots[plot(7, 11)].feature == features::hidden_edge &&
        spatial[plot(7, 11)].blocking_feature
    );
    CHECK(world.plots[plot(6, 12)].feature == features::hidden_edge);
    std::cout << "mission features follow the map passed\n";
}

// Once every feature is placed, the schema's as well as the map's, each
// indestructible feature with metal paints the low byte of that metal over
// its footprint. The match plots hold it too, and a new extractor's rate is
// its type's rate times the sum of each footprint plot's metal plus one. A
// saved game's restored metal takes the place of the painted metal.
void schema_metal_features_paint_their_metal() {
    auto options = feature_options();
    constexpr uint16_t ore = 7;
    auto& def = options.features.emplace_back();
    std::snprintf(def.name, sizeof def.name, "%s", "Ore");
    def.footprint_x = def.footprint_z = 3;
    def.flags = OA_FEATURE_FLAG_SPRITE | OA_FEATURE_FLAG_INDESTRUCTIBLE;
    def.seq_name = standing;
    def.metal = 479.0F; // low byte 223
    set_links(def, features::no_feature, features::no_feature, features::no_feature);
    features::FeaturePlacement entry{};
    std::snprintf(entry.name, sizeof entry.name, "%s", "Ore");
    entry.x = 9;
    entry.z = 9;
    options.mission_features = {entry};
    Fixture f(options);
    const auto& world = f.match->state();
    const auto& spatial = f.match->spatial().plots;
    CHECK(world.plots[plot(9, 9)].feature == ore);
    for (const auto cell : {plot(9, 9), plot(11, 11)})
        CHECK(world.plots[cell].metal == 223 && spatial[cell].metal == 223);
    CHECK(world.plots[plot(12, 11)].metal == 0 && spatial[plot(12, 11)].metal == 0);
    // The rock's metal is reclaimed, not painted: it is not indestructible.
    CHECK(world.plots[plot(4, 4)].metal == 0);
    f.def.extracts_metal = 0.5F;
    CHECK(f.spawn(0, 10 * 16 + 8, 10 * 16 + 8).record.extracted_metal == 0.5F * 224);
    CHECK(f.spawn(0, 2 * 16 + 8, 2 * 16 + 8).record.extracted_metal == 0.5F);
    f.match->state().plots[plot(2, 3)].metal = 40;
    f.match->adopt_plot_metal();
    CHECK(spatial[plot(2, 3)].metal == 40 && spatial[plot(10, 10)].metal == 223);
    CHECK(f.spawn(1, 2 * 16 + 8, 3 * 16 + 8).record.extracted_metal == 0.5F * 41);
    std::cout << "schema metal features paint their metal passed\n";
}

// The schema load reads waterdoesdamage and waterdamage from
// the GlobalHeader; the unit tick applies them.
void water_damage_comes_from_the_header() {
    Fixture dry;
    CHECK(
        dry.match->state().environment_enabled == 0 && dry.match->state().environment_damage == 0
    );
    auto options = feature_options();
    options.water_does_damage = 1;
    options.water_damage = 12;
    Fixture wet(options);
    CHECK(
        wet.match->state().environment_enabled == 1 && wet.match->state().environment_damage == 12
    );
    std::cout << "water damage comes from the header passed\n";
}

// The game loop steps the storm once a tick; its hits are unowned
// projectiles carrying the velocity they were launched with.
void meteor_storm_steps_every_tick() {
    Fixture f(feature_options());

    struct Storm {
        Fixture* fixture;
        uint32_t steps;
    } storm{&f, 0};

    f.match->meteor = {&storm, [](void* context) {
                           auto& self = *static_cast<Storm*>(context);
                           ++self.steps;
                           if (self.steps == 2)
                               (void)self.fixture->match->launch_meteor(
                                   oa::oa_ref_from_index(registry_gun),
                                   {0x400000, 0x5460000, 0x400000},
                                   {0, static_cast<oa_fixed>(0xfff10000u), 0},
                                   true
                               );
                       }};
    f.run(1);
    CHECK(storm.steps == 1 && f.match->state().game.projectile_count == 0);
    f.run(1);
    const auto& world = f.match->state();
    CHECK(storm.steps == 2 && world.game.projectile_count == 1);
    const auto& shot = world.projectiles[0];
    CHECK(
        shot.owner_index == 10 && shot.source == 0 &&
        shot.velocity.y == static_cast<oa_fixed>(0xfff10000u)
    );
    CHECK(shot.created_tick == world.game.tick && shot.position.y == 0x5460000);
    std::cout << "meteor storm steps every tick passed\n";
}

// A level shot of TESTGUN, 12 above the high ground, flown until it bursts;
// the burst's point.
FixedVec3 fly_level_shot(Fixture& f, int32_t x, int32_t z, int32_t velocity_x) {
    constexpr int32_t shot_height = high_ground + 12;
    auto& world = f.match->state();
    const FixedVec3 from{x << 16, shot_height << 16, z << 16};
    const FixedVec3 velocity{velocity_x << 16, 0, 0};
    auto* shot = sim::weapon_execution::spawn_free_projectile(
        world, oa::oa_ref_from_index(registry_gun), from, velocity
    );
    CHECK(shot != nullptr);
    shot->lifetime_tick = world.game.tick + 200;
    shot->feature_cell_x = shot->feature_cell_z = -1;
    const auto logged = f.match->effects().explosion_count;
    for (int32_t step = 0; step < 6 && world.game.projectile_count != 0; ++step) {
        ++world.game.tick;
        f.match->update_projectiles();
    }
    CHECK(world.game.projectile_count == 0 && f.match->effects().explosion_count == logged + 1);
    const auto burst = f.match->effects().explosions[logged].position;
    CHECK(burst.y == shot_height << 16);
    return burst;
}

// A 16-AoE shot entering a 20-high wreck's footprint 12 above the ground
// bursts on the feature with no target (projectile contact), and the area
// blast reaches only half the AoE. It measures the
// wreck's origin plot from the record's point, where the dead unit stood, and
// a continuation plot from the footprint centre for that plot,
// both at ground height: hits on the west plot's edge and on the east plot
// leave the wreck untouched. The same blast 4 above the record's point
// damages it.
void edge_hits_leave_a_wreck_whole() {
    auto options = feature_options();
    options.high_ground_from = 0;
    Fixture f(options);
    allow_feature_damage(f);
    auto& world = f.match->state();
    constexpr int32_t wreck_x = 6, wreck_z = 4;
    const FixedVec3 stood{(wreck_x * 16 + 16) << 16, high_ground << 16, (wreck_z * 16 + 16) << 16};
    const int16_t orientation[3]{};
    const auto* placed = features::place_feature(
        world, f.match->feature_host(), plot(wreck_x, wreck_z), wreck, &stood, orientation, 1
    );
    CHECK(placed != nullptr && record_at(f, wreck_x, wreck_z) == placed);
    const auto& spatial = f.match->spatial().plots;
    CHECK(spatial[plot(wreck_x, wreck_z)].feature_height == 20);
    CHECK(spatial[plot(wreck_x + 1, wreck_z)].feature_word == features::feature_continuation);
    arm_gun(f, 16, 100, false);
    const auto west = fly_level_shot(f, wreck_x * 16 - 6, wreck_z * 16 + 8, 4);
    CHECK(west.x == (wreck_x * 16 + 2) << 16 && west.z == (wreck_z * 16 + 8) << 16);
    CHECK(placed->damage == 0);
    // 12 from the east plot's centre (x 128, z 80), which lies on the footprint's edge.
    const auto east = fly_level_shot(f, wreck_x * 16 + 38, wreck_z * 16 + 15, -4);
    CHECK(east.x == (wreck_x * 16 + 30) << 16 && east.z == (wreck_z * 16 + 15) << 16);
    CHECK(world.plots[plot(wreck_x, wreck_z)].feature == wreck && placed->damage == 0);
    blast_at(f, {stood.x, (high_ground + 4) << 16, stood.z});
    CHECK(placed->damage == 100);
    std::cout << "edge hits leave a wreck whole passed\n";
}

// feature_host hands weapon hits on features and the feature changes settled
// here to the match's multiplayer hooks. A hit the hook says another
// player's machine settles leaves the feature as it was, whether it comes
// from a blast or straight from the feature code; a hit settled here applies,
// and its destruction, a finished reclaim (with the reclaiming unit's slot)
// and a fire started here are reported with their cells. The other cases run
// with those entries null, which applies every hit here and reports nothing.
void feature_hits_and_changes_reach_the_multiplayer_hooks() {
    Fixture f(feature_options());
    allow_feature_damage(f);

    struct Hit {
        uint16_t weapon_id{};
        int32_t cell_x{}, cell_z{};
    };

    struct Change {
        features::FeatureChange change{};
        int32_t cell_x{}, cell_z{};
        uint16_t reclaimer{};
    };

    struct Forwarded {
        bool elsewhere{};
        std::vector<Hit> hits;
        std::vector<Change> changes;
    } forwarded;

    auto& hooks = f.match->multiplayer;
    hooks.context = &forwarded;
    hooks.feature_hit_elsewhere = [](void* context, uint16_t weapon_id, int32_t x, int32_t z) {
        auto& self = *static_cast<Forwarded*>(context);
        self.hits.push_back({weapon_id, x, z});
        return self.elsewhere;
    };
    hooks.feature_changed =
        [](
            void* context, features::FeatureChange change, int32_t x, int32_t z, uint16_t reclaimer
        ) { static_cast<Forwarded*>(context)->changes.push_back({change, x, z, reclaimer}); };
    const auto hit_at = [&forwarded](std::size_t index, uint16_t weapon_id, int32_t x, int32_t z) {
        const auto& hit = forwarded.hits.at(index);
        return hit.weapon_id == weapon_id && hit.cell_x == x && hit.cell_z == z;
    };
    const auto change_at = [&forwarded](
                               std::size_t index,
                               features::FeatureChange change,
                               int32_t x,
                               int32_t z,
                               uint16_t reclaimer = 0
                           ) {
        const auto& reported = forwarded.changes.at(index);
        return reported.change == change && reported.cell_x == x && reported.cell_z == z &&
               reported.reclaimer == reclaimer;
    };
    auto& world = f.match->state();
    arm_gun(f, 32, 30, false);
    const auto& gun = world.game.weapon_defs[registry_gun];
    const FixedVec3 at_far_tree{(3 * 16 + 8) << 16, 0, (12 * 16 + 8) << 16};

    forwarded.elsewhere = true;
    blast_at(f, at_far_tree);
    features::damage_feature(world, f.match->feature_host(), plot(3, 12), 3, 12, gun);
    CHECK(forwarded.hits.size() == 2);
    CHECK(hit_at(0, gun.weapon_id, 3, 12) && hit_at(1, gun.weapon_id, 3, 12));
    CHECK(world.plots[plot(3, 12)].feature_record == 0 && record_at(f, 3, 12) == nullptr);
    CHECK(forwarded.changes.empty());

    forwarded.elsewhere = false;
    blast_at(f, at_far_tree);
    CHECK(forwarded.hits.size() == 3 && world.plots[plot(3, 12)].feature_record == 30);
    CHECK(forwarded.changes.empty());
    features::damage_feature(world, f.match->feature_host(), plot(3, 12), 3, 12, gun);
    const auto* dying_tree = record_at(f, 3, 12);
    CHECK(dying_tree && dying_tree->sprite.animation.sequence == dying);
    CHECK(forwarded.changes.size() == 1);
    CHECK(change_at(0, features::FeatureChange::destroyed, 3, 12));

    auto& unit = f.spawn(0, 64, 96);
    CHECK(f.match->reclaim_feature(unit.record, {(4 * 16 + 8) << 16, 0, (4 * 16 + 8) << 16}));
    CHECK(forwarded.changes.size() == 2);
    CHECK(change_at(1, features::FeatureChange::reclaimed, 4, 4, unit.unit_index));

    blast(f, 8, 8, 100, true);
    CHECK(record_at(f, 8, 8) && (record_at(f, 8, 8)->state & features::state_burning) != 0);
    CHECK(forwarded.hits.size() == 5 && hit_at(4, gun.weapon_id, 8, 8));
    CHECK(forwarded.changes.size() == 3);
    CHECK(change_at(2, features::FeatureChange::ignited, 8, 8));
    std::cout << "feature hits and changes reach the multiplayer hooks passed\n";
}

// A hit by weapon 300 is forwarded as 300, not as the low byte 44.
void wide_weapon_hit_forwards_its_whole_id() {
    Options options = feature_options();
    options.gun_id = 300;
    options.weapon_types = 1024;
    Fixture f(options);
    allow_feature_damage(f);

    struct Hit {
        uint16_t weapon_id{};
        int32_t cell_x{}, cell_z{};
    };

    struct Forwarded {
        std::vector<Hit> hits;
    } forwarded;

    auto& hooks = f.match->multiplayer;
    hooks.context = &forwarded;
    hooks.feature_hit_elsewhere = [](void* context, uint16_t weapon_id, int32_t x, int32_t z) {
        static_cast<Forwarded*>(context)->hits.push_back({weapon_id, x, z});
        return true;
    };
    const auto* gun = oa::world_weapon_def(&f.match->state(), oa::oa_ref_from_index(300));
    CHECK(gun != nullptr && oa::oa_weapon_def_id(gun) == 300);
    features::damage_feature(f.match->state(), f.match->feature_host(), plot(3, 12), 3, 12, *gun);
    CHECK(forwarded.hits.size() == 1 && forwarded.hits[0].weapon_id == 300);
    CHECK(forwarded.hits[0].cell_x == 3 && forwarded.hits[0].cell_z == 12);
    std::cout << "wide weapon hit forwards its whole id passed\n";
}

} // namespace

int main() {
    try {
        reclaimed_rock_plays_its_sequence();
        burning_tree_spreads();
        destroyed_tree_leaves_its_remnant();
        vent_smokes_every_five_ticks();
        mission_features_follow_the_map();
        schema_metal_features_paint_their_metal();
        water_damage_comes_from_the_header();
        meteor_storm_steps_every_tick();
        edge_hits_leave_a_wreck_whole();
        feature_hits_and_changes_reach_the_multiplayer_hooks();
        wide_weapon_hit_forwards_its_whole_id();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
