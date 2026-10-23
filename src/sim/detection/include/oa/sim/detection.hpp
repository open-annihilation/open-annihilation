// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Radar, sonar and line-of-sight contacts of the viewpoint player: the
// per-unit steps of the contact scan that decides which other players' units
// the viewer sees on radar and which it sees below the waterline. A unit whose
// top is under the sea is drawn and targetable by an enemy only while it is a
// sonar contact.
//
// Each player also keeps sightings of other players' units, rebuilt every 30
// ticks; target searches draw on them, and the contact scan uncloaks a unit
// while an enemy its owner sees is inside its mincloakdistance.
#pragma once

#include "oa/core/world.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace oa::sim::detection {

// Unit.flags bits the scan owns.
inline constexpr uint32_t radar_contact = OA_UNIT_FLAG_RADAR_CONTACT;
// Core names the bit for its first writer: the scan sets it on the viewer's
// own and radar-sharing units as well as on sonar contacts.
inline constexpr uint32_t sonar_contact = OA_UNIT_FLAG_VIEWPOINT_OWNED;
inline constexpr uint32_t jammed = OA_UNIT_FLAG_JAMMED;
inline constexpr uint32_t contact_bits = radar_contact | sonar_contact | jammed;

/// Bits of one unit slot in a player's radar picture.
///
/// They are not `Unit.flags` values. `picture_radar` matches a radar contact,
/// `picture_sonar` a sonar contact and `picture_jammed` a jammed unit.
inline constexpr uint8_t picture_radar = 1;
inline constexpr uint8_t picture_sonar = 2;
inline constexpr uint8_t picture_jammed = 4;

// Player-info role bit an owner sets to share its radar with allies.
inline constexpr uint8_t share_radar_role = 0x40;

// Unit.flags bits of the cloak: the Cloak_On order sets `cloak_ordered`, and
// the scan sets `cloak_locked` until its next pass while an enemy is close.
inline constexpr uint32_t cloak_ordered = OA_UNIT_FLAG_CLOAK_RUNNING;
inline constexpr uint32_t cloak_locked = OA_UNIT_FLAG_CLOAK_LOCKED;
// Unit.flags bit that keeps a unit off every player's seen list.
inline constexpr uint32_t unsighted = OA_UNIT_FLAG_NOT_SELECTABLE;
// UnitDef.flags of a repair pad: a builder that is an air base.
inline constexpr uint32_t repair_pad_flags = OA_UNIT_DEF_FLAG_BUILDER | OA_UNIT_DEF_FLAG_IS_AIRBASE;
// How far a damaged aircraft looks for a repair pad: world units, on the
// ground plane.
inline constexpr int32_t repair_pad_reach = 0xf00;

// Ticks between rebuilds of a player's sightings.
inline constexpr uint32_t sighting_period = 30;
// Ticks an enemy inside mincloakdistance holds the cloak off.
inline constexpr uint32_t decloak_hold_ticks = 90;

// Reach and position of one scanning unit, as the contact scan walks it.
struct ScanRecord {
    int32_t radar_range_squared{}; // world units squared
    int32_t sonar_range_squared{};
    FixedVec3 position{};
};

// One player's sightings of other players' units, by unit slot: a seen list
// and a radar list with their counts, and with them the player's own repair
// pads. The caller owns the list storage, `capacity` slots for each list.
struct Sightings {
    uint16_t* seen{};  // enemies in the player's line of sight
    uint16_t* radar{}; // enemies carrying the radar-contact bit
    // The player's own finished, switched-on air bases that are builders,
    // in unit slot order: the repair pads its damaged aircraft choose among
    // until the next rebuild.
    uint16_t* pads{};
    uint32_t capacity{};
    uint32_t seen_count{};
    uint32_t radar_count{};
    uint32_t pad_count{};
    uint32_t refreshed_tick{};
    // The player owns a finished, switched-on targeting facility: target
    // searches that find nothing seen fall back to the radar list.
    uint8_t radar_fallback{};
};

/// One known unit in a list kept in unit slot order.
struct SightedUnit {
    uint16_t unit{};       ///< the unit's slot
    uint32_t generation{}; ///< the generation that tells this unit from the next to take the slot
};

/// Compares two lists of known units and reports what entered and what left.
///
/// Both lists are in ascending slot order. An entry matches another only when
/// its slot and its generation both match. Each entry only the earlier list
/// holds is reported lost, and each only the later list holds is reported
/// gained, in slot order. Where one slot is in both lists with different
/// generations, the lost entry is reported before the gained one. Nothing is
/// allocated.
///
/// @tparam Entry a type with unit and generation members
/// @tparam Lost a callable taking one earlier-list entry
/// @tparam Gained a callable taking one later-list entry
/// @param before the earlier list
/// @param after the later list
/// @param lost called with each entry only before holds
/// @param gained called with each entry only after holds
template <class Entry, class Lost, class Gained>
void diff_sighted(
    std::span<const Entry> before, std::span<const Entry> after, Lost&& lost, Gained&& gained
) {
    std::size_t before_at = 0;
    std::size_t after_at = 0;
    while (before_at < before.size() || after_at < after.size()) {
        const bool before_left = before_at < before.size();
        const bool after_left = after_at < after.size();
        if (before_left && after_left && before[before_at].unit == after[after_at].unit &&
            before[before_at].generation == after[after_at].generation) {
            ++before_at;
            ++after_at;
            continue;
        }
        // The same slot with a new generation is lost before it is gained.
        if (before_left && (!after_left || before[before_at].unit <= after[after_at].unit)) {
            lost(before[before_at]);
            ++before_at;
            continue;
        }
        gained(after[after_at]);
        ++after_at;
    }
}

/// Returns the squared horizontal distance between two points as the game sums it.
///
/// @param a first point, 16.16 world coordinates
/// @param b second point, 16.16 world coordinates
/// @return the high 32 bits of each 64-bit square of the 16.16 delta, Z first, added with
///         32-bit wraparound; Y is ignored
int32_t squared_distance_high(const FixedVec3& a, const FixedVec3& b) noexcept;

/// Tests whether an owner shares its radar with a viewer.
///
/// @param world player records
/// @param viewer player looking
/// @param owner player whose radar may be shared
/// @return true when the owner's alliance row names the viewer and the owner's player-info
///         role has the share-radar bit
bool shares_radar(World& world, const Player& viewer, const Player& owner) noexcept;

/// Stamps one unit inside a scanner's walk.
///
/// A live, non-stealth unit of another player is a sonar contact at or below sea level
/// inside the sonar reach, and a radar contact when its top is at or above sea level
/// inside the radar reach.
///
/// @param scan scanning unit's reach and position
/// @param[in,out] unit unit walked over; gains contact bits
/// @param world sea level and unit types
void stamp_contact(const ScanRecord& scan, Unit& unit, const World& world) noexcept;

/// Marks a unit inside a radar jammer's walk: it leaves radar and is marked jammed.
///
/// @param[in,out] unit unit walked over
void jam_radar(Unit& unit) noexcept;

/// Marks a unit inside a sonar jammer's walk: it leaves sonar and is marked jammed.
///
/// @param[in,out] unit unit walked over
void jam_sonar(Unit& unit) noexcept;

/// Stamps one unit inside a scanner's walk into a picture byte.
///
/// The tests are stamp_contact's, with `viewer` in place of the viewpoint:
/// a non-stealth unit of another player is a sonar contact at or below sea
/// level inside the sonar reach, and a radar contact when its top is at or
/// above sea level inside the radar reach. The unit's flags are not written.
///
/// @param scan scanning unit's reach and position
/// @param viewer player whose picture is being built
/// @param unit unit walked over
/// @param world sea level and unit types
/// @param[in,out] bits the unit's picture byte
void stamp_picture(
    const ScanRecord& scan, uint8_t viewer, const Unit& unit, const World& world, uint8_t& bits
) noexcept;

/// Marks a picture byte inside a radar jammer's walk: it leaves radar and is marked jammed.
///
/// @param[in,out] bits the unit's picture byte
void jam_picture_radar(uint8_t& bits) noexcept;

/// Marks a picture byte inside a sonar jammer's walk: it leaves sonar and is marked jammed.
///
/// @param[in,out] bits the unit's picture byte
void jam_picture_sonar(uint8_t& bits) noexcept;

/// Tells whether a jammer jams the viewing player's picture.
///
/// With allied jammers ignored, the viewer's own jammer and a jammer whose
/// owner the viewer's alliance row names do not jam. Otherwise every jammer
/// does. The row is read one way. A watcher and a slotless viewer are not
/// special here.
///
/// @param allied_jammers_ignored intel.allied-jammers-ignored is on
/// @param viewer the viewing player of this pass
/// @param jammer the jamming unit
/// @return true when it jams
bool seat_jammer_jams(
    bool allied_jammers_ignored, const Player& viewer, const Unit& jammer
) noexcept;

/// Returns the picture bits a new unit starts with.
///
/// Sonar when the unit's owner is the viewer, and neither radar nor jammed.
/// That is how a new unit's flags start, with the viewpoint as the viewer.
///
/// @param unit the new unit
/// @param viewer player whose picture is being built
/// @return the unit's starting picture byte
uint8_t created_picture_bits(const Unit& unit, uint8_t viewer) noexcept;

/// Tests whether a player's sightings are due for a rebuild.
///
/// @param sightings the player's sightings
/// @param tick current game tick
/// @return true once a full 30-tick period has passed since the last rebuild
bool sightings_due(const Sightings& sightings, uint32_t tick) noexcept;

/// Empties the sighting lists, the pad list and the radar fallback before a rebuild.
///
/// @param[in,out] sightings the player's sightings
void clear_sightings(Sightings& sightings) noexcept;

/// Tests whether a unit is live, not waiting to die, and finished.
///
/// @param unit unit to test
/// @return true when finished; an unordered (NaN) build fraction counts as finished
bool finished_unit(const Unit& unit) noexcept;

/// Tests whether the sightings rebuild asks whether a player sees a unit.
///
/// @param world player records
/// @param player player whose sightings are rebuilt
/// @param unit unit walked over
/// @return true when the unit is active and its owner is outside the player's alliance row
bool sighting_candidate(const World& world, const Player& player, const Unit& unit) noexcept;

/// Files one unit slot into a player's sightings.
///
/// An active enemy enters the seen list when `seen` holds and it is not unsighted, and
/// the radar list when it is a radar contact. The player's own finished, switched-on
/// targeting facility turns the radar fallback on, and its own finished, switched-on
/// air base that is a builder enters the pad list.
///
/// @param[in,out] sightings the player's sightings
/// @param world unit table and types
/// @param player player whose sightings are rebuilt
/// @param unit unit walked over
/// @param seen result of the player's sight test of the unit
/// @return true for an active, finished unit of the player's own, which the same walk
///         counts into the player's computer knowledge
bool file_sighting(
    Sightings& sightings, const World& world, const Player& player, const Unit& unit, bool seen
) noexcept;

/// Counts the repair pads a damaged aircraft of the player may make for.
///
/// Each unit of the player's pad list, as its last rebuild filed it, counts while it is
/// still a switched-on air base that is a builder, within repair_pad_reach of the point.
///
/// @param sightings the player's sightings
/// @param world unit table and types
/// @param at the aircraft's position, 16.16 world coordinates
/// @return the number of pads
uint32_t
pads_in_reach(const Sightings& sightings, const World& world, const FixedVec3& at) noexcept;

/// Returns one of the repair pads pads_in_reach counts.
///
/// @param sightings the player's sightings
/// @param world unit table and types
/// @param at the aircraft's position, 16.16 world coordinates
/// @param index which of them, from 0, in pad list order
/// @return the pad's unit slot, or 0 when there are not that many
uint16_t pad_in_reach(
    const Sightings& sightings, const World& world, const FixedVec3& at, uint32_t index
) noexcept;

/// Tests whether an active unit on the seen list stands within a distance of a position.
///
/// @param sightings the player's sightings
/// @param world unit table
/// @param position centre, 16.16 world coordinates
/// @param distance radius on the ground plane, world units
/// @return true when one does
bool sighted_within(
    const Sightings& sightings, const World& world, const FixedVec3& position, int16_t distance
) noexcept;

/// Holds a cloaker's cloak off while an enemy is inside its mincloakdistance.
///
/// Upkeep stops for 90 ticks, and the lock holds the cloak off until the next scan
/// clears it.
///
/// @param[in,out] unit cloaking unit
/// @param tick current game tick
void expose_cloaker(Unit& unit, uint32_t tick) noexcept;

} // namespace oa::sim::detection
