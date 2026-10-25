// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/feature_runtime.hpp"
#include "oa/base/text.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

using namespace oa;
using namespace oa::sim::feature_runtime;

int failures = 0;

void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

// Sequence refs: 1 burn (3 frames, one-shot), 2 die (2 frames), 3 reclamate (2 frames), 4 looping (2 frames).
struct Recorder {
    uint32_t random_value = 0;
    uint32_t random_limit = 0; // limit of the latest draw
    int footprint_calls = 0;
    int sounds = 0;
    int smoke = 0;
    int fx = 0;
    int burn_weapons = 0;
    int destroyed = 0;
    oa_ref32 next_object = 100;

    struct Change {
        FeatureChange change{};
        int32_t cell_x{}, cell_z{};
        const Unit* reclaimer{};
    };

    std::vector<Change> changes;
    std::vector<uint16_t> hits_elsewhere; // weapon ids handed to feature_hit_elsewhere
    bool settled_elsewhere = false;
    float credited_energy = 0.0f;
    float credited_metal = 0.0f;
};

FeatureHost make_host(Recorder& recorder) {
    FeatureHost host{};
    host.context = &recorder;
    host.random = [](void* context, uint32_t limit) -> uint32_t {
        auto* recorder = static_cast<Recorder*>(context);
        recorder->random_limit = limit;
        return limit == 0 ? 0 : recorder->random_value % limit;
    };
    host.lcg_random = [](void*) -> int32_t { return 0x4000; };
    host.sequence_frame = [](void*, oa_ref32 sequence, uint16_t frame, FeatureSequenceFrame* out) {
        const uint16_t counts[] = {0, 3, 2, 2, 2};
        if (sequence == 0 || sequence > 4)
            return false;
        out->frame_count = counts[sequence];
        out->repeat = sequence == 4 ? 1 : 0;
        out->duration = 2;
        out->width = 8;
        out->height = 12;
        out->origin_x = 4;
        out->origin_y = 10;
        return frame < out->frame_count;
    };
    host.create_object = [](void* context, const FeatureDef*) -> oa_ref32 {
        return static_cast<Recorder*>(context)->next_object++;
    };
    host.destroy_object = [](void* context, oa_ref32) {
        ++static_cast<Recorder*>(context)->destroyed;
    };
    host.footprint_changed = [](void* context, int16_t, int16_t, int16_t, int16_t) {
        ++static_cast<Recorder*>(context)->footprint_calls;
    };
    host.emit_feature_fx = [](void* context, const FixedVec3*, uint32_t) {
        ++static_cast<Recorder*>(context)->fx;
    };
    host.emit_smoke = [](void* context, const FixedVec3*, uint32_t) {
        ++static_cast<Recorder*>(context)->smoke;
    };
    host.play_sound = [](void* context, const char* name, const FixedVec3*) {
        if (std::strcmp(name, "treeburn") == 0)
            ++static_cast<Recorder*>(context)->sounds;
    };
    host.burn_weapon = [](void* context, oa_ref32, const FixedVec3*) {
        ++static_cast<Recorder*>(context)->burn_weapons;
    };
    host.credit_reclaim = [](void* context, Unit*, float energy, float metal) {
        auto* recorder = static_cast<Recorder*>(context);
        recorder->credited_energy += energy;
        recorder->credited_metal += metal;
    };
    host.feature_hit_elsewhere = [](void* context, uint16_t weapon_id, int32_t, int32_t) {
        auto* recorder = static_cast<Recorder*>(context);
        recorder->hits_elsewhere.push_back(weapon_id);
        return recorder->settled_elsewhere;
    };
    host.feature_changed = [](void* context,
                              FeatureChange change,
                              int32_t cell_x,
                              int32_t cell_z,
                              const Unit* reclaimer) {
        static_cast<Recorder*>(context)->changes.push_back({change, cell_x, cell_z, reclaimer});
    };
    return host;
}

// Feature table: 0 tree (sprite, flammable, burn/die), 1 burnt stump, 2 rock (indestructible),
// 3 wreck (3DO, 2x2), 4 heap (featurereclamate target), 5 geovent.
struct Fixture {
    World world{};
    std::vector<MapPlot> plots;
    std::vector<FeatureDef> defs;
    std::vector<PlacedFeature> records;

    explicit Fixture(int32_t width = 16, int32_t height = 16)
        : plots(static_cast<std::size_t>(width * height)), defs(6), records(slot_capacity) {
        world.game.map_width = width;
        world.game.map_height = height;
        world.game.sea_level = 0;
        world.game.gravity = 0x4000;
        world.plots = plots.data();
        for (auto& plot : plots) {
            plot.feature = no_feature;
            plot.height = 20;
            plot.high_height = 20;
            plot.low_height = 20;
        }
        world.feature_defs = defs.data();
        world.feature_def_count = static_cast<uint32_t>(defs.size());
        world.game.feature_def_count = static_cast<int32_t>(defs.size());
        world.placed_features = reinterpret_cast<uint8_t*>(records.data());
        world.placed_feature_count = static_cast<uint32_t>(records.size());
        for (auto& def : defs) {
            def.footprint_x = 1;
            def.footprint_z = 1;
            def.dead_feature = no_feature;
            def.burnt_feature = no_feature;
            def.reclamate_feature = no_feature;
            def.flags = OA_FEATURE_FLAG_SPRITE;
        }
        auto& tree = defs[0];
        tree.flags =
            OA_FEATURE_FLAG_SPRITE | OA_FEATURE_FLAG_FLAMABLE | OA_FEATURE_FLAG_RECLAIMABLE;
        tree.seq_name_burn = 1;
        tree.seq_name_die = 2;
        tree.seq_name_reclamate = 3;
        tree.spark_time = 4; // ticks, short so a test fire spreads within its burn
        tree.spread_chance = 50;
        tree.damage = 30;
        tree.energy = 12.0f;
        tree.metal = 0.5f;
        tree.burn_weapon = 7;
        tree.dead_feature = 4;
        tree.burnt_feature = 1;
        tree.reclamate_feature = 4;
        defs[2].flags = OA_FEATURE_FLAG_SPRITE | OA_FEATURE_FLAG_INDESTRUCTIBLE;
        auto& wreck = defs[3];
        wreck.flags = 0;
        wreck.footprint_x = 2;
        wreck.footprint_z = 2;
        wreck.damage = 100;
        wreck.dead_feature = 4;
        defs[5].flags = OA_FEATURE_FLAG_SPRITE | OA_FEATURE_FLAG_GEOTHERMAL;
        require(init_feature_pool(world), "pool initialises");
        world.game.console_flags = OA_CONSOLE_FLAG_TREE_DEATH;
    }

    std::size_t at(int32_t x, int32_t z) const {
        return static_cast<std::size_t>(z * world.game.map_width + x);
    }
};

void test_pool() {
    Fixture f;
    require(feature_list_head(f.world, FeatureList::free_slots) == 0, "free list starts at 0");
    require(feature_list_head(f.world, FeatureList::active) == no_slot, "active list empty");
    const auto a = alloc_feature_slot(f.world);
    const auto b = alloc_feature_slot(f.world);
    require(a == 0 && b == 1, "slots pop in order");
    require(feature_list_head(f.world, FeatureList::active) == 1, "newest slot heads active list");
    require(f.records[1].next == 0 && f.records[0].prev == 1, "active list linked");
    move_feature_slot(f.world, 0, FeatureList::settled);
    require(feature_list_head(f.world, FeatureList::settled) == 0, "slot moved to settled");
    require(f.records[1].next == no_slot, "unlinked from active");
    move_feature_slot(f.world, 1, FeatureList::free_slots);
    require(feature_list_head(f.world, FeatureList::free_slots) == 1, "freed slot heads free list");
    require(f.records[2].prev == 1, "free list back link");
    for (int32_t i = 0; i < slot_capacity - 1; ++i)
        (void)alloc_feature_slot(f.world);
    require(alloc_feature_slot(f.world) == slot_capacity, "exhausted pool reports capacity");
}

void test_place_and_clear() {
    Fixture f;
    Recorder r;
    const auto host = make_host(r);
    require(
        place_feature(f.world, host, f.at(3, 4), 0, nullptr, nullptr, no_player) == nullptr,
        "sprite feature has no record"
    );
    require(f.plots[f.at(3, 4)].feature == 0, "origin word set");
    require(
        (f.plots[f.at(3, 4)].flags & OA_PLOT_FLAG_PLAYER_FEATURE_MASK) >> plot_player_shift ==
            no_player,
        "player bits hold no player"
    );
    auto* wreck = place_feature(f.world, host, f.at(6, 6), 3, nullptr, nullptr, 2);
    require(
        wreck != nullptr && wreck->model.object == 100, "3DO feature allocates record and object"
    );
    require(
        f.plots[f.at(6, 6)].feature == 3 &&
            (f.plots[f.at(6, 6)].flags & OA_PLOT_FLAG_ANIMATING_FEATURE) != 0,
        "3DO origin marked animating"
    );
    const auto& back = f.plots[f.at(7, 7)];
    require(
        back.feature == feature_continuation && back.feature_record == 0x0101,
        "continuation back offsets"
    );
    require(f.plots[f.at(7, 6)].feature_record == 0x0100, "x offset in high byte");
    const auto center = feature_center(f.world, 6, 6, f.defs[3]);
    require(center.x == (2 + 12) << 19 && center.y == 20 << 16, "footprint centre on ground");
    if (wreck == nullptr)
        return;
    require(
        wreck->model.position.x == center.x && wreck->model.position.z == center.z,
        "record at centre"
    );
    require(r.footprint_calls == 2, "listeners notified per placement");

    require(clear_plot_feature(f.world, host, f.at(7, 7), false), "clear through continuation");
    require(
        f.plots[f.at(6, 6)].feature == no_feature && f.plots[f.at(7, 7)].feature == no_feature,
        "footprint emptied"
    );
    require(
        r.destroyed == 1 && feature_list_head(f.world, FeatureList::free_slots) == 0,
        "record released"
    );

    place_feature(f.world, host, f.at(1, 1), 2, nullptr, nullptr, no_player);
    require(!clear_plot_feature(f.world, host, f.at(1, 1), false), "indestructible kept");
    require(
        place_feature(f.world, host, f.at(1, 1), 0, nullptr, nullptr, no_player) == nullptr &&
            f.plots[f.at(1, 1)].feature == 2,
        "placement over indestructible fails"
    );
    clear_all_features(f.world, host);
    require(
        f.plots[f.at(1, 1)].feature == no_feature && f.plots[f.at(3, 4)].feature == no_feature,
        "clear_all forces everything"
    );

    require(
        place_feature(f.world, host, f.at(15, 15), 3, nullptr, nullptr, no_player) == nullptr &&
            f.plots[f.at(15, 15)].feature == no_feature,
        "footprint past the edge rejected"
    );
    place_feature(f.world, host, f.at(2, 2), 5, nullptr, nullptr, no_player);
    require(r.fx == 1, "geothermal vent queues fx");
    place_feature(f.world, host, f.at(0, 0), feature_marker, nullptr, nullptr, no_player);
    require(f.plots[0].feature == feature_marker, "marker word stored");
}

void test_heights() {
    Fixture f(4, 4);
    f.plots[f.at(1, 1)].height = 0;
    f.plots[f.at(2, 1)].height = 32;
    f.plots[f.at(1, 2)].height = 16;
    f.plots[f.at(2, 2)].height = 48;
    const auto fixed = [](int32_t world) { return static_cast<oa_fixed>(world << 16); };
    require(sample_height(f.world, fixed(16), fixed(16)) == 0, "sample at vertex");
    require(sample_height(f.world, fixed(24), fixed(16)) == 16, "halfway along x");
    require(sample_height(f.world, fixed(24), fixed(24)) == 24, "bilinear centre");
    require(sample_height(f.world, fixed(48), fixed(16)) == -1, "last column outside interior");
    require(sample_height(f.world, fixed(-1), fixed(16)) == -1, "negative outside");
    f.plots[f.at(1, 1)].high_height = 30;
    f.plots[f.at(1, 1)].low_height = 11;
    require(mean_plot_height(f.world, fixed(20), fixed(31)) == 20, "mean of high and low");
    require(mean_plot_height(f.world, fixed(64), fixed(0)) == -1, "mean off map");
    require(plot_height(f.world, 2, 1) == 32 && plot_height(f.world, -1, 0) == 0, "plot height");
}

void test_damage_and_death() {
    Fixture f;
    Recorder r;
    const auto host = make_host(r);
    place_feature(f.world, host, f.at(5, 5), 0, nullptr, nullptr, no_player);
    WeaponDef weapon{};
    weapon.damage_default = 20;
    damage_feature(f.world, host, f.at(5, 5), 5, 5, weapon);
    require(f.plots[f.at(5, 5)].feature_record == 20, "damage kept in plot record word");
    damage_feature(f.world, host, f.at(5, 5), 5, 5, weapon);
    const auto& plot = f.plots[f.at(5, 5)];
    require((plot.flags & OA_PLOT_FLAG_ANIMATING_FEATURE) != 0, "death sequence started");
    const auto* record = feature_record(f.world, plot.feature_record);
    require(
        record != nullptr && record->sprite.animation.sequence == 2 &&
            (record->state & state_burning) == 0,
        "die sequence on record"
    );
    for (int tick = 0; tick < 10; ++tick) {
        f.world.game.tick = static_cast<uint32_t>(tick + 1);
        tick_features(f.world, host);
    }
    require(f.plots[f.at(5, 5)].feature == 4, "featuredead remnant after sequence");

    f.world.game.console_flags &= static_cast<uint16_t>(~OA_CONSOLE_FLAG_TREE_DEATH);
    place_feature(f.world, host, f.at(8, 8), 0, nullptr, nullptr, no_player);
    damage_feature(f.world, host, f.at(8, 8), 8, 8, weapon);
    require(f.plots[f.at(8, 8)].feature_record == 0, "no damage without tree death");
}

void test_fire() {
    Fixture f;
    Recorder r;
    const auto host = make_host(r);
    place_feature(f.world, host, f.at(8, 8), 0, nullptr, nullptr, no_player);
    place_feature(f.world, host, f.at(9, 8), 0, nullptr, nullptr, no_player);
    place_feature(f.world, host, f.at(4, 4), 0, nullptr, nullptr, no_player);
    WeaponDef flamer{};
    flamer.damage_default = 1;
    flamer.fire_starter = 1;
    r.random_value = 1;
    damage_feature(f.world, host, f.at(8, 8), 8, 8, flamer);
    const auto* burning = feature_record(f.world, f.plots[f.at(8, 8)].feature_record);
    require(burning != nullptr && (burning->state & state_burning) != 0, "flame weapon ignites");
    if (burning == nullptr)
        return;
    require(burning->spread_countdown == 3, "countdown is half spark time plus a draw");
    require(
        r.sounds == 1 && r.changes.size() == 1 && r.changes[0].change == FeatureChange::ignited &&
            r.changes[0].cell_x == 8 && r.changes[0].cell_z == 8 &&
            r.changes[0].reclaimer == nullptr,
        "treeburn sound and shared ignition"
    );
    for (uint32_t tick = 1; tick <= 3; ++tick) {
        f.world.game.tick = tick;
        tick_features(f.world, host);
    }
    require(r.smoke >= 1, "burning emits smoke");
    const auto& neighbour = f.plots[f.at(9, 8)];
    const auto* spread = feature_record(f.world, neighbour.feature_record);
    require(
        (neighbour.flags & OA_PLOT_FLAG_ANIMATING_FEATURE) != 0 && spread != nullptr &&
            (spread->state & state_burning) != 0,
        "fire spread to neighbour"
    );
    require(
        (f.plots[f.at(4, 4)].flags & OA_PLOT_FLAG_ANIMATING_FEATURE) == 0, "distant tree untouched"
    );
    require(r.burn_weapons == 1, "burn weapon fired on spread");
    for (uint32_t tick = 4; tick <= 12; ++tick) {
        f.world.game.tick = tick;
        tick_features(f.world, host);
    }
    require(f.plots[f.at(8, 8)].feature == 1, "burnt remnant replaces tree");

    Fixture g;
    Recorder q;
    const auto mirrored_host = make_host(q);
    place_feature(g.world, mirrored_host, g.at(2, 2), 0, nullptr, nullptr, no_player);
    ignite_feature(g.world, mirrored_host, 2, 2, true);
    const auto* mirrored = feature_record(g.world, g.plots[g.at(2, 2)].feature_record);
    require(
        mirrored != nullptr && (mirrored->state & state_no_spread) != 0 && q.changes.empty(),
        "mirrored fire neither spreads nor is shared again"
    );
}

// Every shipped feature has sparktime=5, which loads as 150 ticks. Its fire
// spreads 75 to 149 ticks (2.5 to 5 seconds) after it starts: the countdown is
// half the spark time plus a draw below that half, whatever the draw.
void test_stock_spark_time() {
    constexpr int16_t stock_spark_ticks = 150;
    constexpr uint32_t half = 75;
    constexpr uint32_t seeds = 4096;
    constexpr uint32_t seed_spread = 0x9e3779b1u; // spreads the seeds over 32 bits
    std::vector<bool> seen(256, false);
    bool in_range = true;
    bool drawn_below_half = true;
    for (uint32_t seed = 0; seed < seeds; ++seed) {
        Fixture f;
        f.defs[0].spark_time = stock_spark_ticks;
        Recorder r;
        r.random_value = seed * seed_spread;
        const auto host = make_host(r);
        place_feature(f.world, host, f.at(8, 8), 0, nullptr, nullptr, no_player);
        ignite_feature(f.world, host, 8, 8, false);
        const auto* burning = feature_record(f.world, f.plots[f.at(8, 8)].feature_record);
        if (burning == nullptr || (burning->state & state_burning) == 0) {
            require(false, "stock tree ignites");
            return;
        }
        drawn_below_half = drawn_below_half && r.random_limit == half;
        const uint32_t countdown = burning->spread_countdown;
        in_range = in_range && countdown == half + r.random_value % half;
        seen[countdown] = true;
    }
    require(drawn_below_half, "the draw is below half of 150 ticks");
    require(in_range, "the countdown is 75 ticks plus the draw");
    bool every_countdown = true;
    for (uint32_t countdown = 0; countdown < seen.size(); ++countdown)
        every_countdown =
            every_countdown && seen[countdown] == (countdown >= half && countdown < 2 * half);
    require(every_countdown, "stock fires spread 75 to 149 ticks after they start, and no other");
}

void test_falling_wreck() {
    Fixture f;
    Recorder r;
    const auto host = make_host(r);
    FixedVec3 start{
        static_cast<oa_fixed>(6 << 20),
        static_cast<oa_fixed>(22 << 16),
        static_cast<oa_fixed>(6 << 20)
    };
    auto* wreck = place_feature(f.world, host, f.at(6, 6), 3, &start, nullptr, no_player);
    require(wreck != nullptr, "wreck placed");
    if (wreck == nullptr)
        return;
    wreck->model.velocity.y = -0x8000;
    tick_features(f.world, host);
    require(
        wreck->model.position.y == (22 << 16) - 0x8000 &&
            wreck->model.velocity.y == -0x8000 - 0x4000,
        "gravity accelerates the fall"
    );
    for (int i = 0; i < 8; ++i)
        tick_features(f.world, host);
    require(
        wreck->model.position.y == 20 << 16 && wreck->model.velocity.y == 0,
        "wreck rests on the ground"
    );
    const auto slot = static_cast<int32_t>(f.plots[f.at(6, 6)].feature_record);
    require(feature_list_head(f.world, FeatureList::settled) == slot, "resting wreck settles");

    Fixture sea;
    sea.world.game.sea_level = 30;
    auto* sinking = place_feature(sea.world, host, sea.at(3, 3), 3, &start, nullptr, no_player);
    sinking->model.velocity = {1, -1, 1};
    tick_features(sea.world, host);
    require(
        sinking->model.velocity.x == 0 && sinking->model.velocity.y == wreck_sink_speed &&
            sinking->model.velocity.z == 0,
        "wreck below sea level sinks slowly"
    );
}

void test_drawn_height() {
    Fixture f;
    Recorder r;
    const auto host = make_host(r);
    (void)place_feature(f.world, host, f.at(2, 2), 0, nullptr, nullptr, no_player);
    f.plots[f.at(2, 2)].height = 33;
    require(
        feature_drawn_height(f.world, 2, 2) == 33 << 16,
        "a sprite feature is drawn at its origin plot's height"
    );
    const FixedVec3 start{
        static_cast<oa_fixed>(6 << 20),
        static_cast<oa_fixed>(52 << 16),
        static_cast<oa_fixed>(6 << 20)
    };
    auto* wreck = place_feature(f.world, host, f.at(6, 6), 3, &start, nullptr, no_player);
    require(wreck != nullptr, "wreck placed");
    if (wreck == nullptr)
        return;
    require(
        feature_drawn_height(f.world, 6, 6) == 52 << 16,
        "a wreck is drawn at its record's height, above its plot's"
    );
    wreck->model.position.y = 7 << 16;
    require(
        feature_drawn_height(f.world, 6, 6) == 7 << 16,
        "a wreck is drawn at its record's height, below its plot's"
    );
    require(feature_drawn_height(f.world, -1, 6) == 0, "off the map the height is 0");
}

void test_reproduce_and_reclaim() {
    Fixture f(8, 8);
    Recorder r;
    const auto host = make_host(r);
    f.defs[0].reproduce = 100;
    f.defs[0].reproduce_area = 4;
    place_feature(f.world, host, f.at(3, 3), 0, nullptr, nullptr, no_player);
    set_reproduce_cursor(f.world, static_cast<int32_t>(f.at(3, 3)) + 1);
    r.random_value = 3;
    tick_features(f.world, host);
    require(
        reproduce_cursor(f.world) == static_cast<int32_t>(f.at(3, 3)), "cursor steps back one plot"
    );
    require(f.plots[f.at(4, 4)].feature == 0, "tree seeded at (x+3-2, z+3-2)");
    set_reproduce_cursor(f.world, 0);
    tick_features(f.world, host);
    require(reproduce_cursor(f.world) == 63, "cursor wraps to the last plot");

    Unit unit{};
    const FixedVec3 at{
        static_cast<oa_fixed>((3 << 20) + 5), 0, static_cast<oa_fixed>((3 << 20) + 5)
    };
    require(reclaim_feature(f.world, host, unit, at), "tree reclaimed");
    require(
        r.credited_energy == 12.0f && r.credited_metal == 0.5f, "reclaim credits energy and metal"
    );
    require(
        !r.changes.empty() && r.changes.back().change == FeatureChange::reclaimed &&
            r.changes.back().cell_x == 3 && r.changes.back().cell_z == 3 &&
            r.changes.back().reclaimer == &unit,
        "reclaim shared with the reclaiming unit"
    );
    const auto* record = feature_record(f.world, f.plots[f.at(3, 3)].feature_record);
    require(
        record != nullptr && record->sprite.animation.sequence == 3 &&
            (record->state & state_reclaimed) != 0,
        "reclamate sequence playing"
    );
    require(
        !reclaim_feature(f.world, host, unit, at), "animating sprite cannot be reclaimed twice"
    );
    for (uint32_t tick = 1; tick <= 6; ++tick)
        tick_features(f.world, host);
    require(f.plots[f.at(3, 3)].feature == 4, "featurereclamate remnant");
}

// A hit another player's machine settles leaves the feature alone here; one
// settled here destroys the wreck and shares its die sequence.
void test_hit_elsewhere() {
    Fixture f;
    Recorder r;
    r.settled_elsewhere = true;
    const auto host = make_host(r);
    place_feature(f.world, host, f.at(5, 5), 3, nullptr, nullptr, no_player);
    WeaponDef weapon{};
    weapon.damage_default = 200;
    weapon.weapon_id = 9;
    damage_feature(f.world, host, f.at(5, 5), 5, 5, weapon);
    require(
        r.hits_elsewhere.size() == 1 && r.hits_elsewhere[0] == 9, "hit handed over with its weapon"
    );
    require(
        f.plots[f.at(5, 5)].feature == 3 && r.changes.empty(),
        "hit settled elsewhere is not applied"
    );
    WeaponDef wide{};
    oa_weapon_def_set_id(&wide, 300);
    wide.damage_default = 200;
    damage_feature(f.world, host, f.at(5, 5), 5, 5, wide);
    require(
        r.hits_elsewhere.size() == 2 && r.hits_elsewhere[1] == 300, "weapon 300 is forwarded whole"
    );
    require(f.plots[f.at(5, 5)].feature == 3, "wide hit settled elsewhere is not applied");
    r.settled_elsewhere = false;
    damage_feature(f.world, host, f.at(5, 5), 5, 5, weapon);
    require(f.plots[f.at(5, 5)].feature == 4, "hit settled here destroys wreck");
    require(
        r.changes.size() == 1 && r.changes[0].change == FeatureChange::destroyed &&
            r.changes[0].cell_x == 5 && r.changes[0].cell_z == 5 &&
            r.changes[0].reclaimer == nullptr,
        "destruction shared"
    );
}

void test_placements() {
    Fixture f;
    Recorder r;
    const auto host = make_host(r);
    FeaturePlacement placements[3]{};
    oa::base::text::copy_terminated(placements[0].name, "wreck");
    placements[0].x = 6;
    placements[0].z = 6;
    oa::base::text::copy_terminated(placements[2].name, "tree");
    placements[2].x = 2;
    placements[2].z = 3;
    apply_feature_placements(f.world, host, placements, 3, [](void*, const char* name) -> uint16_t {
        if (std::strcmp(name, "wreck") == 0)
            return 3;
        if (std::strcmp(name, "tree") == 0)
            return 0;
        return no_feature;
    });
    require(f.plots[f.at(5, 5)].feature == 3, "3DO placement centred on its footprint");
    require(f.plots[f.at(2, 3)].feature == 0, "sprite placement at its cell");
}

// void_hidden_edges on a 6x12 map (96x192 world units): the view shows 64x64. The
// bottom pass hides the plot above each plot it tests, so the last row stays.
void test_hidden_edges() {
    for (const bool lava : {false, true}) {
        Fixture f(6, 12);
        auto& game = f.world.game;
        game.map_width_world = 6 * 16;
        game.map_height_world = 12 * 16;
        game.sea_level = 20;
        for (auto& plot : f.plots) {
            plot.height = 0;
            plot.low_height = 30;
        }
        const auto plot = [&f](int32_t x, int32_t z) -> MapPlot& { return f.plots[f.at(x, z)]; };
        plot(1, 0).height = plot(1, 1).height = plot(1, 2).height = 40;
        plot(2, 5).feature = 2;
        plot(3, 5).feature = feature_continuation;
        plot(0, 6).feature = feature_marker;
        plot(0, 2).low_height = 10;
        void_hidden_edges(f.world, lava);
        const auto hidden = [&plot](int32_t x, int32_t z) {
            return plot(x, z).feature == hidden_edge;
        };
        require(game.map_pixel_width == 64 && game.map_pixel_height == 64, "visible extent");
        require(
            hidden(4, 0) && hidden(5, 0) && hidden(4, 11) && hidden(5, 11), "right columns hidden"
        );
        require(!hidden(3, 0) && !hidden(3, 11), "columns left of the edge stay");
        require(
            hidden(1, 0) && hidden(1, 1) && !hidden(1, 2), "top plots drawn above the map hidden"
        );
        require(!hidden(0, 0), "flat top row stays");
        require(hidden(0, 4) && hidden(0, 10), "bottom band hidden");
        require(!hidden(0, 3) && !hidden(0, 11), "last row and the row above the band stay");
        require(plot(2, 5).feature == 2 && hidden(3, 5), "origins stay, continuations hidden");
        require(plot(0, 6).feature == feature_marker, "reserved markers stay");
        require(hidden(0, 2) == lava && !hidden(2, 2), "lava hides plots at or below sea level");
    }
}

} // namespace

int main() {
    test_pool();
    test_place_and_clear();
    test_heights();
    test_damage_and_death();
    test_fire();
    test_stock_spark_time();
    test_falling_wreck();
    test_drawn_height();
    test_reproduce_and_reclaim();
    test_hit_elsewhere();
    test_placements();
    test_hidden_edges();
    if (failures != 0) {
        std::fprintf(stderr, "%d failures\n", failures);
        return EXIT_FAILURE;
    }
    std::puts("feature runtime: ok");
    return EXIT_SUCCESS;
}
