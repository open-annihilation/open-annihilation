// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Each player's radar and sonar picture: the bytes a brain will read, checked
// against the viewpoint's contact flags and against the passes a seat runs.
#include "combat_fixture.hpp"

#include "oa/base/game_loop.hpp"
#include "oa/sim/match_runtime/match_trace.hpp"
#include "oa/sim/state_hash.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

using namespace combat_fixture;
namespace detection = oa::sim::detection;
namespace trace = oa::sim::trace;

/// Returns the options of a match whose sight covers a unit's own cell.
///
/// @param allied_vision turns intel.allied-los-sharing on
/// @param allied_jammers turns intel.allied-jammers-ignored on
/// @return the options
Options picture_options(bool allied_vision, bool allied_jammers) {
    Options options;
    options.sight_cells = 1;
    options.per_player_limit = 12;
    options.rules.intel.allied_los_sharing.enabled = allied_vision;
    options.rules.intel.allied_jammers_ignored.enabled = allied_jammers;
    return options;
}

/// Seats a third player, allied only with itself.
///
/// @param f the fixture
void seat_third_player(Fixture& f) {
    f.match->simulation().players[2].present = true;
    f.match->simulation().players[2].status = OA_PLAYER_STATUS_LOCAL;
    std::array<uint8_t, 10> allies{};
    allies[2] = 1;
    f.match->configure_player_alliances(2, allies);
}

/// Makes one player ally another, one way.
///
/// @param f the fixture
/// @param from the player whose alliance row changes
/// @param to the player it allies
void ally(Fixture& f, uint8_t from, uint8_t to) {
    f.match->state().game.players[from].alliance[to] = 1;
}

/// Creates a unit that holds its fire and is switched off.
///
/// @param f the fixture
/// @param player owner
/// @param x whole world units
/// @param z whole world units
/// @return the unit's slot
sim::unit_spawn::Slot& place(Fixture& f, uint8_t player, uint32_t x, uint32_t z) {
    auto& slot = f.spawn(player, x, z);
    slot.record.flags &= ~OA_UNIT_FLAG_FIRE_ORDER_MASK;
    slot.record.state_flags &= static_cast<uint8_t>(~OA_UNIT_STATE_ACTIVE);
    return slot;
}

/// Gives the fixture's unit type radar, sonar and jammer reaches.
///
/// @param f the fixture
/// @param radar radar distance, world units
/// @param sonar sonar distance, world units
/// @param radar_jam radar jam distance, world units
/// @param sonar_jam sonar jam distance, world units
void give_ranges(Fixture& f, int16_t radar, int16_t sonar, int16_t radar_jam, int16_t sonar_jam) {
    auto& def = f.match->state().unit_defs[1];
    def.radar_distance = radar;
    def.sonar_distance = sonar;
    def.radar_distance_jam = radar_jam;
    def.sonar_distance_jam = sonar_jam;
}

/// Switches a unit on, so its radar and jammer run.
///
/// @param slot the unit
void switch_on(sim::unit_spawn::Slot& slot) {
    slot.record.state_flags |= OA_UNIT_STATE_ACTIVE;
}

/// Copies the plain type onto the reserved def and marks that copy stealth.
///
/// The fixture has one real type. A stealth unit keeps its type index and
/// points its def at the copy, so the plain units stay unstealthed.
///
/// @param f the fixture
void arm_stealth_copy(Fixture& f) {
    auto& defs = f.match->state().unit_defs;
    defs[0] = defs[1];
    defs[0].flags |= OA_UNIT_DEF_FLAG_STEALTH;
}

/// Points a unit at the stealth copy without making it typeless.
///
/// @param slot the unit
void mark_stealth(sim::unit_spawn::Slot& slot) {
    slot.record.def = oa_ref_from_index(0);
}

/// Lowers a unit until its top is under the sea. Horizontal buckets stay put.
///
/// @param f the fixture
/// @param slot the unit
void submerge(Fixture& f, sim::unit_spawn::Slot& slot) {
    const auto sea =
        static_cast<int32_t>(static_cast<uint32_t>(f.match->state().game.sea_level) << 16);
    slot.record.position.y = sea - static_cast<int32_t>(80u << 16);
}

/// Hides a unit from the line-of-sight pass.
///
/// @param slot the unit
void cloak(sim::unit_spawn::Slot& slot) {
    slot.record.state_flags |= OA_UNIT_STATE_CLOAKED;
}

/// Maps a unit's radar, sonar and jammed flags onto picture bits.
///
/// @param flags the unit's flags
/// @return the three picture bits
uint8_t flags_as_picture(uint32_t flags) {
    uint8_t bits = 0;
    if ((flags & OA_UNIT_FLAG_RADAR_CONTACT) != 0)
        bits = static_cast<uint8_t>(bits | detection::picture_radar);
    if ((flags & OA_UNIT_FLAG_VIEWPOINT_OWNED) != 0)
        bits = static_cast<uint8_t>(bits | detection::picture_sonar);
    if ((flags & OA_UNIT_FLAG_JAMMED) != 0)
        bits = static_cast<uint8_t>(bits | detection::picture_jammed);
    return bits;
}

/// Returns one unit's byte in a player's picture.
///
/// @param f the fixture
/// @param player the picture
/// @param slot the unit
/// @return the byte
uint8_t picture_of(const Fixture& f, uint8_t player, const sim::unit_spawn::Slot& slot) {
    const auto bits = f.match->player_contact_bits(player);
    CHECK(slot.unit_index < bits.size());
    return bits[slot.unit_index];
}

/// Checks every live unit's picture byte against its contact flags.
///
/// @param f the fixture
/// @param player the picture, the viewpoint in the equality case
void check_picture_matches_flags(const Fixture& f, uint8_t player) {
    const auto bits = f.match->player_contact_bits(player);
    const auto& world = f.match->state();
    CHECK(bits.size() == world.unit_slot_count);
    for (uint32_t slot = 1; slot < world.unit_slot_count; ++slot) {
        if ((world.units[slot].flags & OA_UNIT_FLAG_LIVE) == 0)
            continue;
        CHECK(bits[slot] == flags_as_picture(world.units[slot].flags));
    }
}

uint8_t radar_and_sonar() {
    return static_cast<uint8_t>(detection::picture_radar | detection::picture_sonar);
}

uint8_t sonar_and_jammed() {
    return static_cast<uint8_t>(detection::picture_sonar | detection::picture_jammed);
}

// Player 0 is the viewpoint, the local player and not a watcher. Its picture,
// kept before the first unit, matches the contact flags after both scans.
void viewpoint_picture_equals_flags() {
    for (const bool allied_vision : {false, true}) {
        for (const bool allied_jammers : {false, true}) {
            for (const bool share : {false, true}) {
                Fixture f(picture_options(allied_vision, allied_jammers));
                const auto& game = f.match->state().game;
                CHECK(game.viewpoint_player == 0);
                CHECK(game.local_player_index == 0);
                const auto* info = oa::world_player_info(&f.match->state(), &game.players[0]);
                CHECK(info != nullptr && (info->options & OA_SETUP_OPTION_WATCHER) == 0);
                give_ranges(f, 80, 50, 30, 0);
                arm_stealth_copy(f);
                f.match->keep_player_contacts(0, true);

                seat_third_player(f);
                auto& scanner = place(f, 0, 96, 96);
                switch_on(scanner);
                auto& spotter = place(f, 0, 200, 96);
                auto& stealth = place(f, 1, 140, 96);
                mark_stealth(stealth);
                auto& cloaked_unit = place(f, 1, 210, 96);
                cloak(cloaked_unit);
                auto& seen = place(f, 1, 221, 96);
                auto& submerged = place(f, 1, 96, 140);
                submerge(f, submerged);
                auto& jammer = place(f, 1, 20, 20);
                switch_on(jammer);
                auto& victim = place(f, 1, 20, 40);
                auto& ally_unit = place(f, 2, 220, 220);
                auto& ally_seen = place(f, 1, 220, 210);
                // Allied after the stamps, so an ally's sight stays on that
                // ally's grid until a deadline rebuilds it.
                ally(f, 2, 0);
                if (allied_jammers)
                    ally(f, 0, 1);
                if (share)
                    f.match->state().player_info[2].role = detection::share_radar_role;

                f.match->scan_contacts();
                f.match->scan_player_contacts(0);
                check_picture_matches_flags(f, 0);

                // A later ally pass jams through the scanner when jammers are
                // not ignored: the scanner is another player's active jammer
                // in that pass. The flags do the same.
                const uint8_t scanner_bits =
                    allied_vision && !allied_jammers ? sonar_and_jammed() : radar_and_sonar();
                CHECK(picture_of(f, 0, scanner) == scanner_bits);
                CHECK(picture_of(f, 0, spotter) == radar_and_sonar());
                CHECK(picture_of(f, 0, stealth) == 0);
                CHECK(picture_of(f, 0, cloaked_unit) == 0);
                CHECK(picture_of(f, 0, seen) == detection::picture_radar);
                CHECK(picture_of(f, 0, submerged) == detection::picture_sonar);
                const bool ally_shown = allied_vision || share;
                CHECK(picture_of(f, 0, ally_unit) == (ally_shown ? radar_and_sonar() : 0));
                CHECK(
                    picture_of(f, 0, ally_seen) == (allied_vision ? detection::picture_radar : 0)
                );
                const bool expect_jammed = !allied_jammers || allied_vision;
                CHECK(picture_of(f, 0, victim) == (expect_jammed ? detection::picture_jammed : 0));

                auto& late_enemy = place(f, 1, 8, 8);
                auto& late_own = place(f, 0, 24, 24);
                CHECK(picture_of(f, 0, late_enemy) == 0);
                CHECK((late_enemy.record.flags & OA_UNIT_FLAG_VIEWPOINT_OWNED) == 0);
                CHECK((late_enemy.record.flags & OA_UNIT_FLAG_RADAR_CONTACT) == 0);
                CHECK((late_enemy.record.flags & OA_UNIT_FLAG_JAMMED) == 0);
                CHECK(picture_of(f, 0, late_own) == detection::picture_sonar);
                CHECK((late_own.record.flags & OA_UNIT_FLAG_VIEWPOINT_OWNED) != 0);
                f.match->scan_contacts();
                f.match->scan_player_contacts(0);
                check_picture_matches_flags(f, 0);
            }
        }
    }
    std::cout << "viewpoint picture equals flags passed\n";
}

/// One jammer case for player 1's picture.
///
/// @param allied_jammers the rule
/// @param viewer_allies_owner player 1's row names the jammer's owner
/// @param owner_allies_viewer the jammer's owner names player 1
/// @param jammed whether the target ends jammed
void expect_jam(
    bool allied_jammers, bool viewer_allies_owner, bool owner_allies_viewer, bool jammed
) {
    Fixture f(picture_options(false, allied_jammers));
    give_ranges(f, 80, 50, 36, 0);
    f.match->keep_player_contacts(1, true);
    auto& scanner = place(f, 1, 96, 96);
    switch_on(scanner);
    auto& target = place(f, 0, 96, 60);
    auto& jammer = place(f, 0, 96, 40);
    switch_on(jammer);
    if (viewer_allies_owner)
        ally(f, 1, 0);
    if (owner_allies_viewer)
        ally(f, 0, 1);
    f.match->scan_player_contacts(1);
    const uint8_t expected = jammed ? detection::picture_jammed : detection::picture_radar;
    CHECK(picture_of(f, 1, target) == expected);
}

// Player 1's picture is not the viewpoint's flags. Its radar stamps player
// 0's units, a player 0 jammer jams by player 1's alliance row, stealth is
// skipped, a cloak blocks line of sight, and sonar stops at the reach.
void other_players_see_their_own_picture() {
    Fixture f(picture_options(false, false));
    give_ranges(f, 80, 50, 36, 0);
    arm_stealth_copy(f);
    f.match->keep_player_contacts(1, true);

    auto& scanner = place(f, 1, 96, 96);
    switch_on(scanner);
    auto& owned_unit = place(f, 1, 96, 116);
    cloak(owned_unit);
    auto& spotter = place(f, 1, 220, 96);
    auto& jammer = place(f, 0, 96, 140);
    switch_on(jammer);
    auto& target = place(f, 0, 96, 60);
    auto& stealth = place(f, 0, 150, 96);
    mark_stealth(stealth);
    auto& cloaked_unit = place(f, 0, 220, 100);
    cloak(cloaked_unit);
    auto& seen = place(f, 0, 221, 96);
    auto& in_sonar = place(f, 0, 60, 96);
    submerge(f, in_sonar);
    auto& out_of_sonar = place(f, 0, 20, 96);
    submerge(f, out_of_sonar);

    f.match->scan_player_contacts(1);
    CHECK(picture_of(f, 1, scanner) == radar_and_sonar());
    CHECK(picture_of(f, 1, owned_unit) == sonar_and_jammed());
    CHECK((picture_of(f, 1, owned_unit) & detection::picture_radar) == 0);
    CHECK(picture_of(f, 1, spotter) == radar_and_sonar());
    CHECK(picture_of(f, 1, target) == detection::picture_radar);
    CHECK(picture_of(f, 1, stealth) == 0);
    CHECK(picture_of(f, 1, cloaked_unit) == 0);
    CHECK(picture_of(f, 1, seen) == detection::picture_radar);
    CHECK(picture_of(f, 1, in_sonar) == detection::picture_sonar);
    CHECK(picture_of(f, 1, out_of_sonar) == 0);

    // The rule follows player 1's row, not player 0's.
    expect_jam(true, false, true, true);
    expect_jam(true, true, false, false);
    expect_jam(false, true, false, true);
    std::cout << "other players see their own picture passed\n";
}

struct UnitSnap {
    uint16_t slot{};
    uint32_t flags{};
    uint8_t state_flags{};
    uint32_t decloak_until_tick{};
    std::array<uint8_t, OA_PLAYER_COUNT> visible{};
};

struct SightSnap {
    std::vector<uint16_t> seen;
    std::vector<uint16_t> radar;
    std::vector<uint16_t> pads;
    uint32_t seen_count{};
    uint32_t radar_count{};
    uint32_t pad_count{};
    uint32_t refreshed_tick{};
    uint8_t radar_fallback{};
};

std::vector<uint16_t> copy_slots(const uint16_t* data, uint32_t count) {
    if (data == nullptr || count == 0)
        return {};
    return {data, data + count};
}

std::vector<UnitSnap> snap_units(const Fixture& f) {
    std::vector<UnitSnap> snaps;
    const auto& world = f.match->state();
    for (uint32_t slot = 1; slot < world.unit_slot_count; ++slot) {
        const auto& unit = world.units[slot];
        if ((unit.flags & OA_UNIT_FLAG_LIVE) == 0)
            continue;
        UnitSnap snap;
        snap.slot = static_cast<uint16_t>(slot);
        snap.flags = unit.flags;
        snap.state_flags = unit.state_flags;
        snap.decloak_until_tick = unit.decloak_until_tick;
        for (uint8_t player = 0; player < OA_PLAYER_COUNT; ++player)
            snap.visible[player] =
                f.match->unit_visible(player, static_cast<uint16_t>(slot)) ? 1 : 0;
        snaps.push_back(snap);
    }
    return snaps;
}

std::array<SightSnap, OA_PLAYER_COUNT> snap_sightings(const Fixture& f) {
    std::array<SightSnap, OA_PLAYER_COUNT> snaps{};
    for (uint8_t player = 0; player < OA_PLAYER_COUNT; ++player) {
        const auto& sightings = f.match->sightings(player);
        snaps[player].seen = copy_slots(sightings.seen, sightings.seen_count);
        snaps[player].radar = copy_slots(sightings.radar, sightings.radar_count);
        snaps[player].pads = copy_slots(sightings.pads, sightings.pad_count);
        snaps[player].seen_count = sightings.seen_count;
        snaps[player].radar_count = sightings.radar_count;
        snaps[player].pad_count = sightings.pad_count;
        snaps[player].refreshed_tick = sightings.refreshed_tick;
        snaps[player].radar_fallback = sightings.radar_fallback;
    }
    return snaps;
}

bool same_units(const std::vector<UnitSnap>& before, const std::vector<UnitSnap>& after) {
    if (before.size() != after.size())
        return false;
    for (std::size_t i = 0; i < before.size(); ++i) {
        if (before[i].slot != after[i].slot || before[i].flags != after[i].flags ||
            before[i].state_flags != after[i].state_flags ||
            before[i].decloak_until_tick != after[i].decloak_until_tick ||
            before[i].visible != after[i].visible)
            return false;
    }
    return true;
}

bool same_sightings(
    const std::array<SightSnap, OA_PLAYER_COUNT>& before,
    const std::array<SightSnap, OA_PLAYER_COUNT>& after
) {
    for (uint8_t player = 0; player < OA_PLAYER_COUNT; ++player) {
        if (before[player].seen != after[player].seen ||
            before[player].radar != after[player].radar ||
            before[player].pads != after[player].pads ||
            before[player].seen_count != after[player].seen_count ||
            before[player].radar_count != after[player].radar_count ||
            before[player].pad_count != after[player].pad_count ||
            before[player].refreshed_tick != after[player].refreshed_tick ||
            before[player].radar_fallback != after[player].radar_fallback)
            return false;
    }
    return true;
}

// A picture scan writes none of the simulation the viewpoint's scan does,
// including a cloaker standing next to an enemy its owner has seen.
void pictures_leave_the_simulation_alone() {
    Options options;
    options.sight_cells = 7;
    Fixture f(options);
    f.match->keep_player_contacts(1, true);
    auto& def = f.match->state().unit_defs[1];
    def.abilities |= OA_UNIT_DEF_ABILITY_CAN_CLOAK;
    def.min_cloak_distance = 64;
    auto& cloaker = place(f, 1, 64, 64);
    (void)place(f, 0, 96, 64);
    f.match->state().game.tick = 30;
    f.match->refresh_player_knowledge(1);
    CHECK(f.match->sightings(1).seen_count == 1);
    cloaker.record.state_flags |= OA_UNIT_STATE_CLOAKED;
    cloaker.record.flags &= ~OA_UNIT_FLAG_CLOAK_LOCKED;
    cloaker.record.decloak_until_tick = 0;
    const auto units_before = snap_units(f);
    const auto sightings_before = snap_sightings(f);
    CHECK(f.match->fault() == nullptr);
    f.match->scan_player_contacts(1);
    CHECK(same_units(units_before, snap_units(f)));
    CHECK(same_sightings(sightings_before, snap_sightings(f)));
    CHECK((cloaker.record.flags & OA_UNIT_FLAG_CLOAK_LOCKED) == 0);
    CHECK(cloaker.record.decloak_until_tick == 0);
    CHECK(f.match->fault() == nullptr);
    std::cout << "pictures leave the simulation alone passed\n";
}

// Player 1's picture advances on player 1's deadline, every 30 ticks from the
// first, and an unkept picture stays empty.
void scans_follow_the_deadline() {
    Fixture f;
    CHECK(f.match->player_contact_bits(10).empty());
    CHECK(!f.match->player_contacts_kept(10));
    CHECK(f.match->player_contacts_tick(10) == 0);
    CHECK(f.match->fault() == nullptr);
    f.match->keep_player_contacts(1, true);
    CHECK(f.match->player_contacts_kept(1));
    CHECK(f.match->player_contact_bits(1).size() == f.match->state().unit_slot_count);
    CHECK(f.match->player_contacts_tick(1) == 0);
    f.run(1);
    CHECK(f.match->player_contacts_tick(1) == 1);
    CHECK(f.match->state().game.players[1].next_economy_tick == 30);
    f.run(28);
    CHECK(f.match->state().game.tick == 29);
    CHECK(f.match->player_contacts_tick(1) == 1);
    f.run(1);
    CHECK(f.match->player_contacts_tick(1) == 30);
    f.run(29);
    CHECK(f.match->state().game.tick == 59);
    CHECK(f.match->player_contacts_tick(1) == 30);
    f.run(1);
    CHECK(f.match->state().game.tick == 60);
    CHECK(f.match->player_contacts_tick(1) == 60);
    CHECK(!f.match->player_contacts_kept(0));
    CHECK(f.match->player_contact_bits(0).empty());
    CHECK(f.match->player_contacts_tick(0) == 0);
    f.match->keep_player_contacts(1, false);
    CHECK(!f.match->player_contacts_kept(1));
    CHECK(f.match->player_contact_bits(1).empty());
    CHECK(f.match->player_contacts_tick(1) == 0);
    f.match->keep_player_contacts(1, true);
    CHECK(f.match->player_contacts_tick(1) == 0);
    f.run(1);
    CHECK(f.match->state().game.tick == 61);
    CHECK(f.match->player_contacts_tick(1) == 0);
    f.match->keep_player_contacts(10, true);
    CHECK(f.match->fault() != nullptr);
    CHECK(
        std::string_view(f.match->fault()).find("outside the ten players") != std::string_view::npos
    );
    CHECK(!f.match->player_contacts_kept(10));
    CHECK(f.match->player_contacts_tick(1) == 0);
    CHECK(f.match->player_contacts_kept(1));
    f.match->clear_fault();
    std::cout << "scans follow the deadline passed\n";
}

struct Digests {
    std::vector<uint64_t> totals;
    uint64_t state{};
};

/// Fights two switched-on units for 40 ticks, optionally keeping both pictures.
///
/// @param keep whether both players' pictures are kept before the units exist
/// @return each tick's total digest and the final state digest
Digests fight(bool keep) {
    Fixture f;
    if (keep) {
        f.match->keep_player_contacts(0, true);
        f.match->keep_player_contacts(1, true);
    }
    give_ranges(f, 400, 200, 400, 200);
    auto& attacker = f.spawn(0, 48, 64);
    auto& defender = f.spawn(1, 96, 64);
    switch_on(attacker);
    switch_on(defender);
    CHECK(f.match->issue_attack(attacker.unit_index, defender.unit_index, true));
    CHECK(f.match->issue_attack(defender.unit_index, attacker.unit_index, true));
    Digests out;
    std::vector<trace::UnitSide> sides;
    for (int step = 0; step < 40; ++step) {
        f.run(1);
        sides.assign(f.match->state().unit_slot_count, {});
        const auto digest = sim::match_runtime::match_tick_digest(*f.match, sides);
        out.totals.push_back(
            digest.sections[static_cast<std::size_t>(trace::Section::total)].value
        );
    }
    const base::game_loop::Timing timing{};
    out.state = trace::match_state_hash(*f.match, timing, 0, 0, nullptr);
    return out;
}

// Keeping pictures through a fight does not move a tick digest or the state digest.
void kept_pictures_change_nothing() {
    const Digests plain = fight(false);
    const Digests kept = fight(true);
    CHECK(plain.totals.size() == 40);
    CHECK(plain.totals == kept.totals);
    CHECK(plain.state == kept.state);
    std::cout << "kept pictures change nothing passed\n";
}

} // namespace

int main() {
    try {
        viewpoint_picture_equals_flags();
        other_players_see_their_own_picture();
        pictures_leave_the_simulation_alone();
        scans_follow_the_deadline();
        kept_pictures_change_nothing();
    } catch (const std::exception& error) {
        std::cerr << "player contacts: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
