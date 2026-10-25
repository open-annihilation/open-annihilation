// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A replayed game's timeline, which the generator plans a script's shots
// from: what the map and its water, the players and the unit and weapon
// types were, every unit created, finished, shooting, hit and killed, where
// the mobile units were every sample_period_ticks ticks and how every player
// stood every stats_period_ticks ticks.
//
// Everything is an integer read from the world as the replay ran, so the
// same recording gives the same timeline on every platform. Positions are
// whole map pixels of the world point (x across, y up, z down the map); the
// map-image row a point is drawn on is z - y / 2 (ground_row).
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace oa::media::director {

/// Ticks between two samples of the mobile units: half a second.
inline constexpr uint32_t sample_period_ticks = 15;
/// Ticks between two samples of the players' standing: a second.
inline constexpr uint32_t stats_period_ticks = 30;
/// Weapons a unit type carries at most.
inline constexpr size_t unit_weapon_slots = 3;
/// A player index that names no player.
inline constexpr uint8_t no_player = 0xff;
/// The side of the squares the water map covers, in map pixels.
inline constexpr int32_t water_square_pixels = 64;

/// A world point in whole map pixels.
struct WorldPoint {
    int32_t x{};
    int32_t y{}; ///< height
    int32_t z{};
};

/// Returns the map-image row a world point is drawn on.
///
/// @param point the point
/// @return point.z - point.y / 2, the division rounding toward zero
[[nodiscard]] constexpr int32_t ground_row(WorldPoint point) noexcept {
    return point.z - point.y / 2;
}

/// A player of the replayed game.
struct TimelinePlayer {
    uint8_t index{};    ///< the player index, 0 to 9
    std::string name{}; ///< as the game shows it
    uint8_t side{};     ///< the setup block's side
    uint8_t color{};    ///< the setup block's colour
    bool watcher{};     ///< the setup block marks the player as only watching
    bool viewer{};      ///< the slot the replay is watched from
    bool owned_units{}; ///< the player owned a unit at some tick
    WorldPoint start{}; ///< the player's start position
    uint16_t allies{};  ///< bit n set: allied with player n
};

/// A unit type the replayed game used.
struct TimelineUnitType {
    uint16_t type_index{};   ///< the type's index in the world's unit types
    std::string unit_name{}; ///< UnitDef.unit_name
    bool mobile{};           ///< not a building (UnitDef.bm_code nonzero)
    bool can_fly{};
    bool builder{};                                    ///< the type builds or assists
    uint32_t metal_cost{};                             ///< UnitDef.build_cost_metal, whole units
    uint32_t energy_cost{};                            ///< UnitDef.build_cost_energy, whole units
    uint32_t max_damage{};                             ///< UnitDef.max_damage: full health
    uint32_t build_distance{};                         ///< map pixels
    std::array<uint16_t, unit_weapon_slots> weapons{}; ///< weapon ids, 0 for none
};

/// A weapon type the replayed game used.
struct TimelineWeapon {
    uint16_t weapon_id{}; ///< the weapon's index in the world's weapon types
    std::string name{};
    int32_t range{};           ///< map pixels
    uint32_t flags{};          ///< WeaponDef.flags
    int32_t area_of_effect{};  ///< map pixels
    int32_t damage{};          ///< WeaponDef.damage_default
    int32_t shake_magnitude{}; ///< 0 for none
};

/// What the replay was and how it ended.
struct TimelineHeader {
    std::string map_name{};
    int32_t map_width{};   ///< Game.map_pixel_width: the width a view may show
    int32_t map_height{};  ///< Game.map_pixel_height
    uint32_t first_tick{}; ///< the tick the replay started on
    uint32_t last_tick{};  ///< the last tick replayed
    uint8_t viewer_player{no_player};
    std::vector<TimelinePlayer> players{};      ///< in player index order
    std::vector<TimelineUnitType> unit_types{}; ///< the types used, in type index order
    std::vector<TimelineWeapon> weapons{};      ///< the weapons used, in weapon id order
    /// The water map: squares of water_square_pixels on the ground plane
    /// (x and ground_row), water_columns across and water_rows down.
    int32_t water_columns{};
    int32_t water_rows{};
    /// Each square's part under the sea, in percent, row by row: of the
    /// map's cells whose middle's ground point lies in the square, those at
    /// or below the sea level; 0 for a square no cell's middle lies in.
    std::vector<uint8_t> water{};
};

/// What an event reports.
enum class EventKind : uint8_t {
    created,    ///< a unit was created: unit, owner, unit_type, at, build_left
    finished,   ///< a unit was finished: unit, owner, unit_type, at, other_unit the builder
    shot,       ///< a shot was placed: unit the shooter (0 for a meteor), owner, weapon,
                ///< at the muzzle, target the aim (when has_target), other_unit the target
    detonation, ///< a shot detonated: unit its shooter, owner, weapon, at, other_unit struck
    damage,     ///< a health event: unit the target, owner its owner, other_unit the
                ///< attacker, other_owner its owner, amount, damage_kind, at the target
    death,      ///< a unit died: unit, owner, unit_type, at, other_unit the last
                ///< attacker, other_owner its owner, damage_kind the death kind
};

/// Bits of TimelineEvent::flags.
namespace event_flag {
inline constexpr uint8_t commander = 1;         ///< the unit is its side's commander
inline constexpr uint8_t has_target = 2;        ///< a shot's target point is known
inline constexpr uint8_t settled_elsewhere = 4; ///< a death the recording settled
inline constexpr uint8_t burst = 8;             ///< a burst shot copied from the one before
} // namespace event_flag

/// One event, as the match's event hooks reported it.
struct TimelineEvent {
    uint32_t tick{}; ///< the tick being run when it happened
    EventKind kind{EventKind::created};
    uint8_t flags{}; ///< event_flag bits
    uint8_t owner{no_player};
    uint8_t other_owner{no_player};
    uint16_t weapon{}; ///< the weapon's id; 0 for none
    uint8_t damage_kind{};
    uint16_t unit{};       ///< unit slot; 0 for none
    uint16_t other_unit{}; ///< unit slot; 0 for none
    uint16_t unit_type{};  ///< type index; 0 for none
    int32_t amount{};      ///< health points
    uint32_t build_left{}; ///< build progress still to go, 0 finished to 65536 not started
    WorldPoint at{};
    WorldPoint target{};
};

/// Bits of UnitSample::flags.
namespace sample_flag {
inline constexpr uint8_t visible = 1; ///< the viewer's view draws the unit
inline constexpr uint8_t cloaked = 2;
inline constexpr uint8_t carried = 4;    ///< attached to another unit
inline constexpr uint8_t unfinished = 8; ///< build progress still to go
} // namespace sample_flag

/// Where one live mobile unit was on a sample tick.
struct UnitSample {
    uint32_t tick{};
    uint16_t unit{};
    uint16_t unit_type{};
    uint8_t owner{no_player};
    uint8_t flags{};    ///< sample_flag bits
    uint16_t heading{}; ///< 65536ths of a turn
    int32_t health{};   ///< health points
    WorldPoint at{};
};

/// How one player stood on a stats tick.
struct PlayerStats {
    uint32_t tick{};
    uint8_t player{};
    uint32_t kills{};
    uint32_t losses{};
    uint32_t commanders_killed{};
    uint32_t commanders_lost{};
    uint32_t unit_count{};
    int32_t metal{};  ///< stored, whole units
    int32_t energy{}; ///< stored, whole units
};

/// How the replay went, as the extension that replayed it reported.
struct ReplayVerdict {
    bool finished{}; ///< everything recorded was replayed
    bool clean{};
    bool paced{};
    bool content_differs{}; ///< the installation's unit definitions differ from the recording's
    uint32_t errors{};
    uint32_t first_error_tick{}; ///< the tick the first error was seen on; 0 for none
    std::string last_error{};
};

/// A whole timeline.
struct Timeline {
    TimelineHeader header{};
    std::vector<TimelineEvent> events{}; ///< in the order they happened
    std::vector<UnitSample> samples{};   ///< by tick, then unit slot
    std::vector<PlayerStats> stats{};    ///< by tick, then player index
    ReplayVerdict verdict{};
    /// The first tick a script may not show: the tick after the last one
    /// replayed, or the first error's tick when the replay was not clean.
    uint32_t usable_end_tick{};
};

} // namespace oa::media::director
