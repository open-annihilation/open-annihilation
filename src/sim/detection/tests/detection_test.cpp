// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/detection.hpp"

#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <span>

using namespace oa;
using namespace oa::sim::detection;

namespace {
int failures = 0;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                           \
            ++failures;                                                                            \
        }                                                                                          \
    } while (false)

constexpr uint8_t sea_level = 100;
constexpr uint8_t viewer = 0;
constexpr uint8_t enemy = 1;
constexpr uint8_t ally = 2;

// Unit types: 1 plain, 2 stealth; both 10 high.
enum : uint16_t { plain = 1, stealthy, type_count };

// Slot 1 is the viewer's, 2..6 the enemy's, 7 the ally's.
constexpr uint32_t viewer_slot = 1, enemy_first = 2, ally_slot = 7, slot_count = 8;

constexpr int32_t fixed(int32_t whole) {
    return static_cast<int32_t>(static_cast<uint32_t>(whole) << 16);
}

World* make_world() {
    World* w = world_create();
    WorldCapacity capacity{slot_count, type_count, 0};
    if (w == nullptr || !world_alloc_tables(w, &capacity))
        std::abort();
    w->game.sea_level = sea_level;
    w->game.viewpoint_player = viewer;
    for (uint16_t t = 1; t < type_count; ++t)
        w->unit_defs[t].model_height = fixed(10);
    w->unit_defs[stealthy].flags = OA_UNIT_DEF_FLAG_STEALTH;
    const auto own = [&](uint8_t player, uint32_t first, uint32_t last) {
        auto& p = w->game.players[player];
        p.in_use = 1;
        p.index = player;
        p.first_unit = oa_unit_ref_from_slot(first);
        p.last_unit = oa_unit_ref_from_slot(last);
        p.info = oa_ref_from_index(player);
        for (uint32_t slot = first; slot <= last; ++slot) {
            w->units[slot].owner = oa_ref_from_index(player);
            w->units[slot].owner_index = player;
        }
    };
    own(viewer, viewer_slot, viewer_slot);
    own(enemy, enemy_first, ally_slot - 1);
    own(ally, ally_slot, slot_count - 1);
    return w;
}

Unit& place(World& w, uint32_t slot, uint16_t type, int32_t x, int32_t y, int32_t z) {
    auto& unit = w.units[slot];
    unit.type_index = type;
    unit.def = oa_ref_from_index(type);
    unit.flags = OA_UNIT_FLAG_LIVE;
    unit.position = {fixed(x), fixed(y), fixed(z)};
    return unit;
}

uint32_t contacts(const Unit& unit) {
    return unit.flags & contact_bits;
}

// Radar reach 100 and sonar reach 50 from the origin.
ScanRecord scanner_at_origin() {
    ScanRecord scan;
    scan.radar_range_squared = 100 * 100;
    scan.sonar_range_squared = 50 * 50;
    scan.position = {0, fixed(sea_level), 0};
    return scan;
}

void test_distance() {
    const FixedVec3 a{fixed(3), fixed(9), fixed(4)};
    const FixedVec3 origin{};
    CHECK(squared_distance_high(a, origin) == 25);
    CHECK(squared_distance_high(origin, a) == 25);
    CHECK(squared_distance_high({fixed(1) / 2, 0, 0}, origin) == 0);
}

void test_submerged_hull_needs_sonar() {
    World* w = make_world();
    const auto scan = scanner_at_origin();
    // Top at 90: under the surface, so never a radar contact.
    auto& near = place(*w, enemy_first, plain, 30, sea_level - 20, 30);
    auto& far = place(*w, enemy_first + 1, plain, 0, sea_level - 20, 50);
    stamp_contact(scan, near, *w);
    stamp_contact(scan, far, *w);
    CHECK(contacts(near) == sonar_contact);
    CHECK(contacts(far) == 0);
    world_destroy(w);
}

void test_surface_and_land_contacts() {
    World* w = make_world();
    const auto scan = scanner_at_origin();
    auto& ship = place(*w, enemy_first, plain, 0, sea_level - 3, 30);
    auto& tank = place(*w, enemy_first + 1, plain, 30, sea_level + 5, 0);
    auto& radar_only = place(*w, enemy_first + 2, plain, 0, sea_level - 3, -70);
    // The top exactly at sea level still shows on radar.
    auto& awash = place(*w, enemy_first + 3, plain, 10, sea_level - 10, 0);
    auto& hidden = place(*w, enemy_first + 4, stealthy, 10, sea_level - 3, 0);
    for (auto* unit : {&ship, &tank, &radar_only, &awash, &hidden})
        stamp_contact(scan, *unit, *w);
    CHECK(contacts(ship) == (sonar_contact | radar_contact));
    CHECK(contacts(tank) == radar_contact);
    CHECK(contacts(radar_only) == radar_contact);
    CHECK(contacts(awash) == (sonar_contact | radar_contact));
    CHECK(contacts(hidden) == 0);
    world_destroy(w);
}

void test_reach_is_exclusive() {
    World* w = make_world();
    const auto scan = scanner_at_origin();
    auto& sonar_edge = place(*w, enemy_first, plain, 50, sea_level - 3, 0);
    auto& radar_edge = place(*w, enemy_first + 1, plain, 0, sea_level + 5, 100);
    stamp_contact(scan, sonar_edge, *w);
    stamp_contact(scan, radar_edge, *w);
    CHECK(contacts(sonar_edge) == radar_contact);
    CHECK(contacts(radar_edge) == 0);
    world_destroy(w);
}

void test_own_and_empty_units_are_skipped() {
    World* w = make_world();
    const auto scan = scanner_at_origin();
    auto& own = place(*w, viewer_slot, plain, 0, sea_level, 10);
    auto& empty = place(*w, enemy_first, plain, 0, sea_level, 10);
    empty.type_index = 0;
    stamp_contact(scan, own, *w);
    stamp_contact(scan, empty, *w);
    CHECK(contacts(own) == 0);
    CHECK(contacts(empty) == 0);
    world_destroy(w);
}

void test_jammers() {
    World* w = make_world();
    auto& radar_jammed = place(*w, enemy_first, plain, 0, sea_level, 0);
    auto& sonar_jammed = place(*w, enemy_first + 1, plain, 0, sea_level, 0);
    radar_jammed.flags |= radar_contact | sonar_contact;
    sonar_jammed.flags |= radar_contact | sonar_contact;
    jam_radar(radar_jammed);
    jam_sonar(sonar_jammed);
    CHECK(contacts(radar_jammed) == (sonar_contact | jammed));
    CHECK(contacts(sonar_jammed) == (radar_contact | jammed));
    CHECK((radar_jammed.flags & OA_UNIT_FLAG_LIVE) != 0);
    world_destroy(w);
}

void test_shared_radar() {
    World* w = make_world();
    const auto& watcher = w->game.players[viewer];
    const auto& friendly = w->game.players[ally];
    CHECK(!shares_radar(*w, watcher, friendly));
    w->game.players[ally].alliance[viewer] = 1;
    CHECK(!shares_radar(*w, watcher, friendly));
    w->player_info[ally].role = share_radar_role;
    CHECK(shares_radar(*w, watcher, friendly));
    w->game.players[ally].alliance[viewer] = 0;
    CHECK(!shares_radar(*w, watcher, friendly));
    world_destroy(w);
}

// Storage for one player's sightings over the fixture's unit slots.
struct SightingLists {
    uint16_t seen[slot_count]{};
    uint16_t radar[slot_count]{};
    uint16_t pads[slot_count]{};
    Sightings sightings{seen, radar, pads, slot_count};
};

// Files slots 1..7 for the viewer; `visible` is the viewer's sight test.
// Returns how many of the viewer's own finished units the walk reported.
uint32_t file_all(SightingLists& lists, World& w, const bool (&visible)[slot_count]) {
    const auto& player = w.game.players[viewer];
    clear_sightings(lists.sightings);
    uint32_t own = 0;
    for (uint32_t slot = 1; slot < slot_count; ++slot) {
        const auto& unit = w.units[slot];
        const bool seen = sighting_candidate(w, player, unit) && visible[slot];
        if (file_sighting(lists.sightings, w, player, unit, seen))
            ++own;
    }
    return own;
}

void test_sightings_cadence() {
    Sightings sightings;
    CHECK(!sightings_due(sightings, 29));
    CHECK(sightings_due(sightings, 30));
    sightings.refreshed_tick = 30;
    CHECK(!sightings_due(sightings, 59));
    CHECK(sightings_due(sightings, 60));
}

void test_sightings_lists() {
    World* w = make_world();
    auto& me = w->game.players[viewer];
    me.alliance[viewer] = 1;
    me.alliance[ally] = 1;
    w->unit_defs[plain].flags = OA_UNIT_DEF_FLAG_TARGETING_UPGRADE;
    // The viewer's finished, switched-on targeting facility.
    auto& facility = place(*w, viewer_slot, plain, 0, sea_level, 0);
    facility.state_flags = OA_UNIT_STATE_ACTIVE;
    auto& spotted = place(*w, enemy_first, plain, 10, sea_level, 0);
    auto& radar_blip = place(*w, enemy_first + 1, plain, 20, sea_level, 0);
    radar_blip.flags |= radar_contact;
    auto& unsightable = place(*w, enemy_first + 2, plain, 30, sea_level, 0);
    unsightable.flags |= unsighted | radar_contact;
    auto& dying = place(*w, enemy_first + 3, plain, 40, sea_level, 0);
    dying.flags |= OA_UNIT_FLAG_DEATH_PENDING | radar_contact;
    // Slot 6 stays empty.
    auto& friendly = place(*w, ally_slot, plain, 50, sea_level, 0);
    friendly.flags |= radar_contact;
    friendly.state_flags = OA_UNIT_STATE_ACTIVE;

    CHECK(!sighting_candidate(*w, me, facility));
    CHECK(sighting_candidate(*w, me, spotted));
    CHECK(!sighting_candidate(*w, me, dying));
    CHECK(!sighting_candidate(*w, me, friendly));

    // Every live unit passes the viewer's sight test except the radar blip.
    const bool visible[slot_count]{false, true, true, false, true, true, true, true};
    SightingLists lists;
    // Only the facility is the viewer's own; the ally's unit is not counted.
    CHECK(file_all(lists, *w, visible) == 1);
    CHECK(lists.sightings.seen_count == 1 && lists.seen[0] == enemy_first);
    CHECK(lists.sightings.radar_count == 2);
    CHECK(lists.radar[0] == enemy_first + 1 && lists.radar[1] == enemy_first + 2);
    CHECK(lists.sightings.radar_fallback == 1);

    // An unfinished facility gives no fallback; a NaN fraction counts as
    // finished, as an unordered compare counts as equal.
    facility.build_remaining = 0.5F;
    CHECK(file_all(lists, *w, visible) == 0);
    CHECK(lists.sightings.radar_fallback == 0);
    CHECK(lists.sightings.seen_count == 1 && lists.sightings.radar_count == 2);
    facility.build_remaining = std::numeric_limits<float>::quiet_NaN();
    CHECK(file_all(lists, *w, visible) == 1);
    CHECK(lists.sightings.radar_fallback == 1);
    // Switched off it gives none, and the ally's switched-on facility never did.
    facility.build_remaining = 0.0F;
    facility.state_flags = 0;
    file_all(lists, *w, visible);
    CHECK(lists.sightings.radar_fallback == 0);
    world_destroy(w);
}

// The pad list holds the player's own finished, live, switched-on air bases
// that are builders, in slot order; a search re-tests each listed pad and
// measures its reach.
void test_pad_list() {
    World* w = make_world();
    // Slots 1 and 2 are the viewer's here, 3..6 the enemy's.
    auto& me = w->game.players[viewer];
    me.alliance[viewer] = 1;
    me.last_unit = oa_unit_ref_from_slot(enemy_first);
    w->units[enemy_first].owner = oa_ref_from_index(viewer);
    w->units[enemy_first].owner_index = viewer;
    w->game.players[enemy].first_unit = oa_unit_ref_from_slot(enemy_first + 1);
    w->unit_defs[plain].flags = repair_pad_flags;
    w->unit_defs[stealthy].flags = OA_UNIT_DEF_FLAG_IS_AIRBASE;
    auto& near_pad = place(*w, viewer_slot, plain, 100, sea_level, 0);
    auto& far_pad = place(*w, enemy_first, plain, 0x1000, sea_level, 0);
    auto& enemy_pad = place(*w, enemy_first + 1, plain, 50, sea_level, 0);
    for (Unit* pad : {&near_pad, &far_pad, &enemy_pad})
        pad->state_flags = OA_UNIT_STATE_ACTIVE;
    const bool visible[slot_count]{};
    SightingLists lists;
    CHECK(file_all(lists, *w, visible) == 2);
    CHECK(lists.sightings.pad_count == 2);
    CHECK(lists.pads[0] == viewer_slot && lists.pads[1] == enemy_first);

    // An unfinished, switched-off, dying or non-builder base is not filed.
    near_pad.build_remaining = 0.5F;
    file_all(lists, *w, visible);
    CHECK(lists.sightings.pad_count == 1 && lists.pads[0] == enemy_first);
    near_pad.build_remaining = 0.0F;
    near_pad.state_flags = 0;
    file_all(lists, *w, visible);
    CHECK(lists.sightings.pad_count == 1);
    near_pad.state_flags = OA_UNIT_STATE_ACTIVE;
    near_pad.flags |= OA_UNIT_FLAG_DEATH_PENDING;
    file_all(lists, *w, visible);
    CHECK(lists.sightings.pad_count == 1);
    near_pad.flags = OA_UNIT_FLAG_LIVE;
    near_pad.type_index = stealthy;
    near_pad.def = oa_ref_from_index(stealthy);
    file_all(lists, *w, visible);
    CHECK(lists.sightings.pad_count == 1);
    near_pad.type_index = plain;
    near_pad.def = oa_ref_from_index(plain);
    file_all(lists, *w, visible);
    CHECK(lists.sightings.pad_count == 2);

    // The search keeps the filed pads in reach that are still switched on,
    // in list order.
    const FixedVec3 origin{0, fixed(sea_level), 0};
    CHECK(pads_in_reach(lists.sightings, *w, origin) == 1);
    CHECK(pad_in_reach(lists.sightings, *w, origin, 0) == viewer_slot);
    CHECK(pad_in_reach(lists.sightings, *w, origin, 1) == 0);
    const FixedVec3 between{fixed(0x800), fixed(sea_level), 0};
    CHECK(pads_in_reach(lists.sightings, *w, between) == 2);
    CHECK(pad_in_reach(lists.sightings, *w, between, 0) == viewer_slot);
    CHECK(pad_in_reach(lists.sightings, *w, between, 1) == enemy_first);
    // 0xf00 away is in reach, a world unit further is not.
    const FixedVec3 edge{fixed(100 + repair_pad_reach), fixed(sea_level), 0};
    CHECK(pads_in_reach(lists.sightings, *w, edge) == 2);
    const FixedVec3 past{fixed(100 - repair_pad_reach - 1), fixed(sea_level), 0};
    CHECK(pads_in_reach(lists.sightings, *w, past) == 0);
    near_pad.state_flags = 0;
    CHECK(pads_in_reach(lists.sightings, *w, between) == 1);
    CHECK(pad_in_reach(lists.sightings, *w, between, 0) == enemy_first);
    world_destroy(w);
}

void test_sighted_within() {
    World* w = make_world();
    auto& far = place(*w, enemy_first, plain, 100, sea_level, 0);
    // 50 units away on the ground plane, whatever its height.
    auto& near = place(*w, enemy_first + 1, plain, 30, sea_level + 400, 40);
    SightingLists lists;
    const FixedVec3 origin{0, fixed(sea_level), 0};
    CHECK(!sighted_within(lists.sightings, *w, origin, 1000));
    lists.seen[0] = enemy_first;
    lists.seen[1] = enemy_first + 1;
    lists.sightings.seen_count = 2;
    CHECK(sighted_within(lists.sightings, *w, origin, 50));
    CHECK(!sighted_within(lists.sightings, *w, origin, 49));
    // A listed unit that has died since the rebuild no longer counts.
    near.flags |= OA_UNIT_FLAG_DEATH_PENDING;
    CHECK(!sighted_within(lists.sightings, *w, origin, 50));
    CHECK(sighted_within(lists.sightings, *w, origin, 100));
    far.flags = 0;
    CHECK(!sighted_within(lists.sightings, *w, origin, 100));
    // Only the seen list counts.
    near.flags = OA_UNIT_FLAG_LIVE;
    lists.radar[0] = enemy_first + 1;
    lists.sightings.radar_count = 1;
    lists.sightings.seen_count = 0;
    CHECK(!sighted_within(lists.sightings, *w, origin, 100));
    world_destroy(w);
}

/// Checks that a unit is active with OA_UNIT_FLAG_LIVE set and
/// OA_UNIT_FLAG_DEATH_PENDING clear, and finished when build_remaining is
/// neither below nor above 0.0, so an unordered fraction reads as zero.
void test_finished_unit() {
    Unit unit{};
    unit.flags = OA_UNIT_FLAG_LIVE;
    CHECK(finished_unit(unit));
    unit.flags = OA_UNIT_FLAG_LIVE | OA_UNIT_FLAG_DEATH_PENDING;
    CHECK(!finished_unit(unit));
    unit.flags = 0x00000000u;
    CHECK(!finished_unit(unit));
    unit.flags = OA_UNIT_FLAG_LIVE;
    unit.build_remaining = 0.5F;
    CHECK(!finished_unit(unit));
    unit.build_remaining = -0.0F;
    CHECK(finished_unit(unit));
    unit.build_remaining = std::numeric_limits<float>::quiet_NaN();
    CHECK(finished_unit(unit));
}

uint8_t picture_of(uint32_t flags) {
    uint8_t bits = 0;
    if ((flags & radar_contact) != 0)
        bits = static_cast<uint8_t>(bits | picture_radar);
    if ((flags & sonar_contact) != 0)
        bits = static_cast<uint8_t>(bits | picture_sonar);
    if ((flags & jammed) != 0)
        bits = static_cast<uint8_t>(bits | picture_jammed);
    return bits;
}

// Heights above, at and below the sea; distances inside and outside sonar,
// plain radar and a lifted radar reach; stealth, plain and typeless; owned
// by the picture's viewer, the viewpoint and a third player.
void test_stamp_picture_grid() {
    World* w = make_world();
    const int32_t heights[] = {sea_level + 8, sea_level, sea_level - 20};
    const int32_t distances[] = {0, 30, 49, 50, 80, 100, 120, 150, 160};
    const int32_t radar_reaches[] = {100 * 100, 150 * 150};
    const uint8_t owners[] = {viewer, enemy, ally};
    const uint16_t types[] = {plain, stealthy, 0};
    const uint32_t seeds[] = {0u, jammed};
    int viewpoint_units_stamped = 0;
    for (const int32_t height : heights)
        for (const int32_t distance : distances)
            for (const int32_t radar_reach : radar_reaches)
                for (const uint8_t owner : owners)
                    for (const uint16_t type : types)
                        for (const uint32_t seed : seeds) {
                            auto& unit = place(
                                *w, enemy_first, type == 0 ? plain : type, distance, height, 0
                            );
                            unit.owner_index = owner;
                            unit.owner = oa_ref_from_index(owner);
                            if (type == 0)
                                unit.type_index = 0;
                            unit.flags = OA_UNIT_FLAG_LIVE | seed;
                            const uint32_t flags_before = unit.flags;
                            ScanRecord scan = scanner_at_origin();
                            scan.radar_range_squared = radar_reach;
                            uint8_t bits = picture_of(seed);
                            stamp_picture(scan, viewer, unit, *w, bits);
                            Unit copy = unit;
                            stamp_contact(scan, copy, *w);
                            CHECK(bits == picture_of(copy.flags));
                            CHECK(unit.flags == flags_before);
                            // A viewer who is not the viewpoint still stamps the
                            // viewpoint's units, and never its own.
                            uint8_t other = picture_of(seed);
                            stamp_picture(scan, enemy, unit, *w, other);
                            CHECK(unit.flags == flags_before);
                            if (owner == enemy) {
                                CHECK(other == picture_of(seed));
                            } else {
                                copy = unit;
                                copy.owner_index = ally;
                                stamp_contact(scan, copy, *w);
                                CHECK(other == picture_of(copy.flags));
                                if (owner == viewer && (other & picture_radar) != 0)
                                    ++viewpoint_units_stamped;
                            }
                        }
    CHECK(viewpoint_units_stamped > 0);
    auto& bare = place(*w, enemy_first, plain, 0, sea_level, 10);
    bare.owner_index = enemy;
    bare.def = 0;
    bare.flags = OA_UNIT_FLAG_LIVE | radar_contact;
    uint8_t bare_bits = picture_radar;
    stamp_picture(scanner_at_origin(), viewer, bare, *w, bare_bits);
    CHECK(bare_bits == picture_radar);
    CHECK(bare.flags == (OA_UNIT_FLAG_LIVE | radar_contact));
    world_destroy(w);
}

void test_jam_picture_and_seat_rule() {
    uint8_t radar_bits = static_cast<uint8_t>(picture_radar | picture_sonar);
    uint8_t sonar_bits = radar_bits;
    jam_picture_radar(radar_bits);
    jam_picture_sonar(sonar_bits);
    CHECK(radar_bits == static_cast<uint8_t>(picture_sonar | picture_jammed));
    CHECK(sonar_bits == static_cast<uint8_t>(picture_radar | picture_jammed));
    Unit untouched{};
    untouched.flags = OA_UNIT_FLAG_LIVE | radar_contact | sonar_contact;
    CHECK(untouched.flags == (OA_UNIT_FLAG_LIVE | radar_contact | sonar_contact));

    World* w = make_world();
    auto& jammer = place(*w, enemy_first, plain, 0, sea_level, 0);
    const auto& watcher = w->game.players[viewer];
    jammer.owner_index = viewer;
    CHECK(seat_jammer_jams(false, watcher, jammer));
    CHECK(!seat_jammer_jams(true, watcher, jammer));
    jammer.owner_index = enemy;
    CHECK(seat_jammer_jams(false, watcher, jammer));
    CHECK(seat_jammer_jams(true, watcher, jammer));
    jammer.owner_index = ally;
    // The viewer's row names the owner. The owner's row does not name the viewer.
    w->game.players[viewer].alliance[ally] = 1;
    CHECK(seat_jammer_jams(false, watcher, jammer));
    CHECK(!seat_jammer_jams(true, watcher, jammer));
    // The other way: the owner's row names the viewer, and the viewer's does not.
    w->game.players[viewer].alliance[ally] = 0;
    w->game.players[ally].alliance[viewer] = 1;
    CHECK(seat_jammer_jams(false, watcher, jammer));
    CHECK(seat_jammer_jams(true, watcher, jammer));
    jammer.owner_index = OA_PLAYER_RECORD_COUNT;
    CHECK(seat_jammer_jams(false, watcher, jammer));
    CHECK(seat_jammer_jams(true, watcher, jammer));
    world_destroy(w);
}

void test_created_picture_bits() {
    Unit unit{};
    unit.owner_index = viewer;
    CHECK(created_picture_bits(unit, viewer) == picture_sonar);
    CHECK(created_picture_bits(unit, enemy) == 0);
    unit.owner_index = enemy;
    CHECK(created_picture_bits(unit, viewer) == 0);
    CHECK(created_picture_bits(unit, enemy) == picture_sonar);
    unit.flags = radar_contact | sonar_contact | jammed;
    CHECK(created_picture_bits(unit, enemy) == picture_sonar);
}

// One call diff_sighted made, in the order it made them.
struct DiffCall {
    bool gained{};
    uint16_t unit{};
    uint32_t generation{};
    uint16_t mark{};
};

// Appends one call. The lists under test stay within a handful of entries.
template <class Entry>
void note_diff(DiffCall* log, int& count, bool gained, const Entry& entry, uint16_t mark) {
    CHECK(count < 8);
    log[count++] = DiffCall{gained, entry.unit, entry.generation, mark};
}

void test_diff_sighted() {
    DiffCall log[8]{};
    int count = 0;
    const auto lost = [&](const auto& entry) { note_diff(log, count, false, entry, 0); };
    const auto gained = [&](const auto& entry) { note_diff(log, count, true, entry, 0); };
    const auto calls = [&]() {
        return std::span<const DiffCall>{log, static_cast<std::size_t>(count)};
    };

    diff_sighted(std::span<const SightedUnit>{}, std::span<const SightedUnit>{}, lost, gained);
    CHECK(count == 0);

    const SightedUnit two[] = {{1, 4}, {3, 5}};
    count = 0;
    diff_sighted(std::span<const SightedUnit>{}, std::span<const SightedUnit>{two}, lost, gained);
    CHECK(count == 2);
    CHECK(log[0].gained && log[0].unit == 1 && log[0].generation == 4);
    CHECK(log[1].gained && log[1].unit == 3 && log[1].generation == 5);

    count = 0;
    diff_sighted(std::span<const SightedUnit>{two}, std::span<const SightedUnit>{}, lost, gained);
    CHECK(count == 2);
    CHECK(!log[0].gained && log[0].unit == 1 && log[0].generation == 4);
    CHECK(!log[1].gained && log[1].unit == 3 && log[1].generation == 5);

    // Disjoint slots, then a shared slot between them, then a slot only the
    // later list holds, then one only the earlier list holds.
    const SightedUnit before[] = {{1, 1}, {4, 1}, {6, 1}};
    const SightedUnit after[] = {{2, 1}, {4, 2}, {5, 1}};
    count = 0;
    diff_sighted(
        std::span<const SightedUnit>{before}, std::span<const SightedUnit>{after}, lost, gained
    );
    const DiffCall interleaved[] = {
        {false, 1, 1, 0},
        {true, 2, 1, 0},
        {false, 4, 1, 0},
        {true, 4, 2, 0},
        {true, 5, 1, 0},
        {false, 6, 1, 0},
    };
    CHECK(calls().size() == 6);
    for (int index = 0; index < count; ++index) {
        CHECK(log[index].gained == interleaved[index].gained);
        CHECK(log[index].unit == interleaved[index].unit);
        CHECK(log[index].generation == interleaved[index].generation);
    }

    // The same slot with a new generation is lost, then gained.
    const SightedUnit old_slot[] = {{2, 1}};
    const SightedUnit new_slot[] = {{2, 2}};
    count = 0;
    diff_sighted(
        std::span<const SightedUnit>{old_slot}, std::span<const SightedUnit>{new_slot}, lost, gained
    );
    CHECK(count == 2);
    CHECK(!log[0].gained && log[0].unit == 2 && log[0].generation == 1);
    CHECK(log[1].gained && log[1].unit == 2 && log[1].generation == 2);

    count = 0;
    diff_sighted(
        std::span<const SightedUnit>{two}, std::span<const SightedUnit>{two}, lost, gained
    );
    CHECK(count == 0);

    // An entry with a field past the slot and generation: the template reads
    // those two, and the callable still sees the rest. A changed extra field
    // on a matching slot is not a change.
    struct Marked {
        uint16_t unit{};
        uint32_t generation{};
        uint16_t mark{};
    };

    const Marked marked_before[] = {{2, 1, 9}, {4, 3, 8}};
    const Marked marked_after[] = {{2, 1, 7}, {4, 4, 6}};
    count = 0;
    diff_sighted(
        std::span<const Marked>{marked_before},
        std::span<const Marked>{marked_after},
        [&](const Marked& entry) { note_diff(log, count, false, entry, entry.mark); },
        [&](const Marked& entry) { note_diff(log, count, true, entry, entry.mark); }
    );
    CHECK(count == 2);
    CHECK(!log[0].gained && log[0].unit == 4 && log[0].generation == 3 && log[0].mark == 8);
    CHECK(log[1].gained && log[1].unit == 4 && log[1].generation == 4 && log[1].mark == 6);
}

void test_expose_cloaker() {
    Unit unit{};
    unit.flags = OA_UNIT_FLAG_LIVE | cloak_ordered;
    expose_cloaker(unit, 1000);
    CHECK(unit.decloak_until_tick == 1090);
    CHECK(unit.flags == (OA_UNIT_FLAG_LIVE | cloak_ordered | cloak_locked));
}

} // namespace

int main() {
    test_distance();
    test_submerged_hull_needs_sonar();
    test_surface_and_land_contacts();
    test_reach_is_exclusive();
    test_own_and_empty_units_are_skipped();
    test_jammers();
    test_stamp_picture_grid();
    test_jam_picture_and_seat_rule();
    test_created_picture_bits();
    test_shared_radar();
    test_sightings_cadence();
    test_sightings_lists();
    test_pad_list();
    test_sighted_within();
    test_expose_cloaker();
    test_diff_sighted();
    test_finished_unit();
    if (failures)
        std::fprintf(stderr, "%d detection checks failed\n", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
