// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "oa/core/unit.h"
#include "oa/core/unit_def.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "oa/core/weapon_def.h"
#include "oa/data/defs/weapons.hpp"

namespace oa::sim::combat_state {
inline constexpr size_t weapon_slot_count = 3;
inline constexpr int32_t simulation_ticks_per_second = 30;
inline constexpr int32_t milliseconds_per_second = 1000;
inline constexpr double weapon_range_scale = 1.25;
// Bits of WeaponDef.flags, under the names the combat code uses for them.
inline constexpr uint32_t weapon_line_of_sight_flag = OA_WEAPON_FLAG_LINE_OF_SIGHT;
inline constexpr uint32_t weapon_ballistic_flag = OA_WEAPON_FLAG_BALLISTIC;
inline constexpr uint32_t weapon_vlaunch_flag = OA_WEAPON_FLAG_VLAUNCH;
inline constexpr uint32_t weapon_guidance_flag = OA_WEAPON_FLAG_GUIDANCE;
inline constexpr uint32_t weapon_selfprop_flag = OA_WEAPON_FLAG_SELF_PROP;
inline constexpr uint32_t weapon_burnblow_flag = OA_WEAPON_FLAG_BURN_BLOW;
inline constexpr uint32_t weapon_paralyzer_flag = OA_WEAPON_FLAG_PARALYZER;
inline constexpr uint32_t weapon_ground_skip_flag = OA_WEAPON_FLAG_UNITS_ONLY;
inline constexpr uint32_t weapon_ground_bounce_flag = OA_WEAPON_FLAG_GROUND_BOUNCE;
inline constexpr uint32_t weapon_water_flag = OA_WEAPON_FLAG_WATER_WEAPON;
inline constexpr uint32_t weapon_to_air_flag = OA_WEAPON_FLAG_TO_AIR_WEAPON;
inline constexpr uint32_t weapon_turret_flag = OA_WEAPON_FLAG_TURRET;
inline constexpr uint32_t weapon_commandfire_flag = OA_WEAPON_FLAG_COMMAND_FIRE;
inline constexpr uint32_t weapon_beam_flag = OA_WEAPON_FLAG_BEAM_WEAPON;
inline constexpr uint32_t weapon_meteor_flag = OA_WEAPON_FLAG_METEOR;
inline constexpr uint32_t weapon_dropped_flag = OA_WEAPON_FLAG_DROPPED;
inline constexpr uint32_t weapon_start_smoke_flag = OA_WEAPON_FLAG_START_SMOKE;
inline constexpr uint32_t weapon_end_smoke_flag = OA_WEAPON_FLAG_END_SMOKE; // a puff where it ends
inline constexpr uint32_t weapon_sound_trigger_flag = OA_WEAPON_FLAG_SOUND_TRIGGER;
inline constexpr uint32_t weapon_tracks_flag = OA_WEAPON_FLAG_TRACKS;
inline constexpr uint32_t weapon_smoke_trail_flag = OA_WEAPON_FLAG_SMOKE_TRAIL;
inline constexpr uint32_t weapon_propeller_flag = OA_WEAPON_FLAG_PROPELLER;
inline constexpr uint32_t weapon_two_phase_flag = OA_WEAPON_FLAG_TWO_PHASE;
inline constexpr uint32_t weapon_cruise_flag = OA_WEAPON_FLAG_CRUISE;
inline constexpr uint32_t weapon_stockpile_flag = OA_WEAPON_FLAG_STOCKPILE;
inline constexpr uint32_t weapon_targetable_flag = OA_WEAPON_FLAG_TARGETABLE;
inline constexpr uint32_t weapon_shell_flag = OA_WEAPON_FLAG_SHELL_WEAPON;
inline constexpr uint32_t weapon_no_radar_flag = OA_WEAPON_FLAG_NO_RADAR; // no radar blip
// Flies on through its blasts.
inline constexpr uint32_t weapon_no_explode_flag = OA_WEAPON_FLAG_NO_EXPLODE;
inline constexpr uint32_t weapon_noautorange_flag = OA_WEAPON_FLAG_NO_AUTO_RANGE;

// A weapon as the combat code reads it, converted from the WeaponDef record
// its slot was loaded into (WeaponRegistry::install).
struct WeaponDefinition {
    uint16_t reload_time_ticks{};
    uint16_t default_damage{}; // WeaponDef.damage_default, TDF [DAMAGE] default
    uint16_t registry_index{};
    int32_t projectile_velocity{};        // WeaponDef.weapon_velocity, 16.16 world units per tick
    float minimum_barrel_angle_radians{}; // WeaponDef.min_barrel_angle
    int32_t range_world_units{0x7fff};    // WeaponDef.range
    uint32_t flags{};
    float energy_per_shot{};      // WeaponDef.energy_per_shot, TDF energypershot
    float metal_per_shot{};       // WeaponDef.metal_per_shot, TDF metalpershot
    uint16_t weapontimer_ticks{}; // WeaponDef.weapon_timer, TDF weapontimer seconds * 30
    uint16_t areaofeffect{};      // WeaponDef.area_of_effect
    int16_t accuracy{};           // TDF accuracy, WeaponDef.accuracy
    uint16_t tolerance{};         // TDF tolerance, WeaponDef.tolerance
    uint16_t pitch_tolerance{};   // TDF pitchtolerance, WeaponDef.pitch_tolerance
    int32_t start_velocity{};     // TDF startvelocity, WeaponDef.start_velocity
    int32_t acceleration{};       // TDF weaponacceleration, WeaponDef.weapon_acceleration
    uint16_t turn_rate{};         // TDF turnrate / 30, angle units per tick, WeaponDef.turn_rate
    uint16_t burst{};             // TDF burst, WeaponDef.burst
    uint16_t burst_rate_ticks{};  // TDF burstrate seconds * 30, WeaponDef.burst_rate
    uint8_t rendertype{};         // WeaponDef.render_type
    uint8_t color{};              // WeaponDef.color
    uint8_t color2{};             // WeaponDef.color2
    // GAF archives and sequences of the explosions, loaded into
    // WeaponDef.explosion_art and WeaponDef.water_explosion_art.
    std::string explosion_gaf; // TDF explosiongaf, opened as anims/<name>
    std::string explosion_art; // TDF explosionart, the sequence in that archive
    // TDF waterexplosiongaf and waterexplosionart; on a lava world the weapons
    // are loaded with lavaexplosiongaf and lavaexplosionart here instead.
    std::string water_explosion_gaf;
    std::string water_explosion_art;
    std::string soundstart;         // TDF soundstart= wav stem
    std::string soundhit;           // WeaponDef.sound_hit: where it goes off on land or on a unit
    std::string soundwater;         // WeaponDef.sound_water: where it goes off in the water
    int32_t shake_magnitude{};      // TDF shakemagnitude, WeaponDef.shake_magnitude
    int32_t shake_duration_ticks{}; // TDF shakeduration seconds * 30, WeaponDef.shake_duration
    float edge_effectiveness{};     // TDF edgeeffectiveness, WeaponDef.edge_effectiveness
    uint16_t spray_angle{};         // TDF sprayangle, WeaponDef.spray_angle
    uint16_t duration_ticks{};      // TDF duration seconds * 30, WeaponDef.duration
    uint16_t random_decay_ticks{};  // TDF randomdecay seconds * 30, WeaponDef.random_decay
    uint16_t flight_time_ticks{};   // TDF flighttime seconds * 30, WeaponDef.flight_time
    uint16_t smoke_delay_ticks{};   // TDF smokedelay seconds * 30, WeaponDef.smoke_delay
    int32_t coverage{};             // TDF coverage, WeaponDef.coverage: an interceptor's half-width

    // TDF [DAMAGE] entries other than `default`, keyed by lower-case unit name
    // (the table WeaponDef.damage_overrides refers to). A hit uses the entry
    // for the target's UNITNAME.
    struct DamageOverride {
        std::string unit_name;
        int32_t amount{};
    };

    std::vector<DamageOverride> damage_overrides;
};

/// Returns a weapon's damage against one unit type: its [DAMAGE] entry, else the default.
///
/// @param definition weapon whose overrides are searched
/// @param unit_name target's UNITNAME, compared case-insensitively (ASCII)
/// @return the override amount, else `default_damage`
[[nodiscard]] int32_t
damage_against(const WeaponDefinition& definition, std::string_view unit_name) noexcept;

// The reload countdown has no copy here: it lives only in the canonical
// UnitWeapon.reload, which saves, traces and the state digest read.
struct WeaponSlot {
    const WeaponDefinition* definition{};
    // Copied to UnitWeapon.muzzle_offset: 1.25 times the Z of the QueryWeapon
    // piece less that of the AimFrom piece, 16.16 world units.
    int32_t muzzle_offset{};
    uint8_t stockpile{};
    uint8_t flags{};
};

// The piece positions a slot's muzzle offset is taken from; only their Z is used.
struct SlotGeometry {
    int32_t muzzle_z{};   // QueryWeapon piece, 16.16 world units
    int32_t aim_from_z{}; // AimFrom piece, 16.16 world units
};

struct UnitWeapons {
    std::array<WeaponSlot, weapon_slot_count> slots{};
    std::array<const WeaponDefinition*, weapon_slot_count> definitions{};
};

struct InitializationResult {
    int32_t maximum_reload_milliseconds{};
};

/// Advances a self-propelled shot's speed by one tick of acceleration.
///
/// @param speed current speed, 16.16 world units per tick
/// @param maximum TDF weaponvelocity, 16.16 world units per tick
/// @param step TDF weaponacceleration per tick, 16.16
/// @return the new speed, clamped to `maximum`
/// @quirk Both comparisons are unsigned, so a negative speed counts as above any maximum and holds.
[[nodiscard]] inline int32_t
accelerate_projectile(int32_t speed, int32_t maximum, int32_t step) noexcept {
    if (static_cast<uint32_t>(speed) < static_cast<uint32_t>(maximum)) {
        auto next = static_cast<uint32_t>(speed) + static_cast<uint32_t>(step);
        if (static_cast<uint32_t>(maximum) < next)
            next = static_cast<uint32_t>(maximum);
        return static_cast<int32_t>(next);
    }
    return speed;
}

/// Turns one aim axis toward a desired angle by at most one step.
///
/// @param current current angle, 65536 per full turn
/// @param desired angle to reach, same units
/// @param rate largest step per call, same units; zero holds the current angle
/// @return `desired` when it is less than `rate` away, else `current` moved `rate` toward it
[[nodiscard]] inline uint16_t
turn_toward_angle(uint16_t current, uint16_t desired, uint16_t rate) noexcept {
    const auto delta = static_cast<int16_t>(static_cast<uint16_t>(desired - current));
    const auto magnitude = std::abs(static_cast<int>(delta));
    if (magnitude < static_cast<int>(rate))
        return desired;
    if (delta < 0)
        return static_cast<uint16_t>(current - rate);
    return static_cast<uint16_t>(current + rate);
}

/// Computes the random aim spread of a shot from a unit's health and experience.
///
/// The width is `accuracy + 0x800 - health * 0x800 / maximum_health`, divided by
/// the veteran divisor when it exceeds 1.
///
/// @param accuracy TDF accuracy of the weapon, angle units
/// @param health unit's current health
/// @param maximum_health unit type's maximum health; zero gives no spread
/// @param veteran_divisor what the unit's experience divides the width by: its kills
///        / 12 in 3.1c (sim::unit_health::veteran_accuracy_divisor)
/// @return spread width in angle units (65536 per turn), wrapped to 16 bits
[[nodiscard]] inline uint16_t accuracy_spread(
    int16_t accuracy, int16_t health, uint32_t maximum_health, int32_t veteran_divisor
) noexcept {
    if (maximum_health == 0)
        return 0;
    const auto scaled = static_cast<uint32_t>(static_cast<int32_t>(health) << 11) / maximum_health;
    auto spread = static_cast<uint16_t>(
        static_cast<int32_t>(accuracy) - static_cast<int16_t>(scaled) + 0x800
    );
    if (veteran_divisor > 1)
        spread = static_cast<uint16_t>(static_cast<int32_t>(spread) / veteran_divisor);
    return spread;
}

/// Arms a unit's three weapon slots from definitions already resolved.
///
/// Each slot loses its stockpile, takes its definition, keeps flag bits
/// 0xf0, gains the initialized bit, its slot index in bits 2..3 and the
/// non-default-weapon bit, and gets its muzzle offset from `geometry`. The
/// reload countdown is the canonical slot's, which the caller clears.
///
/// @param[in,out] unit slots to arm; `unit.definitions` must all be non-null
/// @param geometry QueryWeapon and AimFrom piece positions for each slot
/// @return the slowest slot's reload in milliseconds (ticks * 1000 / 30)
[[nodiscard]] InitializationResult initialize_weapon_slots(
    UnitWeapons& unit, const std::array<SlotGeometry, weapon_slot_count>& geometry
) noexcept;

inline constexpr size_t weapon_registry_capacity = 256;

/// One weapon definition per slot, from 0 to the table's last.
///
/// A new registry holds weapon_registry_capacity slots. The definitions,
/// names and records are kept in allocations of their own, so a registry is
/// a few bytes wherever it is placed, on a thread's stack too. Growing moves
/// every definition, so grow before any is bound. Copying or assigning a
/// registry, from a temporary too, copies the slots into the target's own
/// storage.
class WeaponRegistry {
  public:

    /// The name stored for one slot.
    using SlotName = std::string;

    /// Creates empty definitions, each carrying its own registry index.
    ///
    /// @param capacity slots to hold; a new registry holds weapon_registry_capacity
    explicit WeaponRegistry(std::size_t capacity = weapon_registry_capacity);

    /// Creates a registry holding a copy of every slot of another.
    ///
    /// @param other registry whose definitions, names and records are copied
    WeaponRegistry(const WeaponRegistry& other);

    /// Copies every slot of another registry over this one's.
    ///
    /// @param other registry whose definitions, names and records are copied
    /// @return this registry
    WeaponRegistry& operator=(const WeaponRegistry& other);

    /// Moves the slots. The moved-from registry holds nothing.
    ///
    /// @param other registry whose slots are taken
    WeaponRegistry(WeaponRegistry&& other) noexcept;

    /// Moves the slots. The moved-from registry holds nothing.
    ///
    /// @param other registry whose slots are taken
    /// @return this registry
    WeaponRegistry& operator=(WeaponRegistry&& other) noexcept;

    /// Returns how many slots the registry holds.
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    /// Grows the registry, keeping every slot and numbering the new ones.
    ///
    /// Growing moves every definition, so grow before any definition is bound.
    ///
    /// @param capacity new slot count; ignored when the registry is already that large
    void grow(std::size_t capacity);

    /// Installs one loaded weapon slot under its section name.
    ///
    /// The definition takes the record's values as they are: velocities in
    /// 16.16 world units per tick, times in ticks, minbarrelangle in radians.
    /// The record itself is kept for the World's weapon tables (see records()).
    /// An id past the registry's capacity grows it first.
    ///
    /// @param weapon slot of a data::defs::WeaponTable; its id is the registry
    ///     index and WeaponDef.key the name
    /// @param assets explosion and sound names the slot's section gave
    void install(const WeaponDef& weapon, const data::defs::WeaponAssetNames& assets);

    /// Adds or replaces one [DAMAGE] entry of a weapon.
    ///
    /// @param index registry index of the weapon
    /// @param unit_name target UNITNAME; stored lower-case
    /// @param amount damage against that unit
    void install_damage_override(uint16_t index, std::string_view unit_name, int32_t amount);

    /// Finds an installed weapon by its internal name.
    ///
    /// The lowest slot wins when several share a name.
    ///
    /// @param name weapon name, compared case-insensitively (ASCII)
    /// @return the definition, or null for an empty or unknown name
    [[nodiscard]] const WeaponDefinition* find(std::string_view name) const noexcept;

    /// Returns record zero, the fallback for unresolved weapon names.
    [[nodiscard]] const WeaponDefinition& default_definition() const noexcept {
        return definitions_[0];
    }

    /// Returns the definition at a registry index.
    [[nodiscard]] const WeaponDefinition& definition(uint16_t index) const noexcept {
        return definitions_[index];
    }

    /// Returns the internal name at a registry index, empty when none is installed.
    [[nodiscard]] std::string_view name(uint16_t index) const noexcept { return names_[index]; }

    /// Returns the weapon records, one per slot.
    ///
    /// A slot nothing was installed in is zeroed apart from its id bytes.
    [[nodiscard]] std::span<const WeaponDef> records() const noexcept {
        return {records_.get(), capacity_};
    }

  private:

    /// Allocates `capacity` zeroed slots and numbers each one.
    ///
    /// @param capacity slots to hold
    void allocate(std::size_t capacity);

    std::unique_ptr<WeaponDefinition[]> definitions_;
    std::unique_ptr<SlotName[]> names_;
    std::unique_ptr<WeaponDef[]> records_;
    std::size_t capacity_{};
};

/// Installs every loaded slot of a weapon table, with its [DAMAGE] entries.
///
/// A slot is loaded when its section name (WeaponDef.key) is set; the others
/// are left as they were.
///
/// @param[in,out] registry registry the slots are installed into
/// @param table weapons as data::defs::load_weapon_defs or load_weapon_text left them
/// @return the number of slots installed
size_t install_weapon_table(WeaponRegistry& registry, const data::defs::WeaponTable& table);

/// Loads every WEAPONS\*.TDF in file order and installs the slots they name.
///
/// The files are read by data::defs::load_weapon_defs.
///
/// @param[in,out] registry registry the slots are installed into
/// @param files file boundary over the game data
/// @param lava_world whether lavaexplosion* keys stand in for waterexplosion*
/// @param weapon_types IDs from 0 to weapon_types - 1 take a slot
/// @return the number of slots installed
size_t install_weapon_files(
    WeaponRegistry& registry,
    const data::defs::Files& files,
    bool lava_world = false,
    uint32_t weapon_types = OA_WEAPON_DEF_COUNT
);

/// Loads weapon TDF text and installs the slots it names.
///
/// The text is read by data::defs::load_weapon_text into a table of its own,
/// so slots it does not name keep what the registry held.
///
/// @param[in,out] registry registry the slots are installed into
/// @param text weapon TDF text
/// @param lava_world whether lavaexplosion* keys stand in for waterexplosion*
/// @param weapon_types IDs from 0 to weapon_types - 1 take a slot
/// @return the number of slots installed; 0 when the text does not parse
size_t install_weapon_text(
    WeaponRegistry& registry,
    std::string_view text,
    bool lava_world = false,
    uint32_t weapon_types = OA_WEAPON_DEF_COUNT
);

struct WeaponBinding {
    UnitWeapons weapons;
    bool resolved_nondefault_weapon{}; // sets OA_UNIT_DEF_FLAG_HAS_WEAPONS
};

/// Resolves a unit type's three weapon names against the registry.
///
/// @param registry installed weapons
/// @param names FBI weapon1, weapon2 and weapon3
/// @return the definitions, with record zero for an empty or unknown name, and whether
///         any name resolved to a weapon other than record zero
[[nodiscard]] WeaponBinding bind_unit_weapons(
    const WeaponRegistry& registry, const std::array<std::string_view, weapon_slot_count>& names
) noexcept;

// The script and model operations a new unit's weapon setup needs: the QueryWeapon
// and AimFrom pieces in world coordinates, and the SetMaxReloadTime call.
struct SpawnGeometryHost {
    virtual ~SpawnGeometryHost() = default;
    /// Runs QueryWeapon for a slot and returns that piece's world position.
    ///
    /// @param slot weapon slot 0..2
    /// @return world position, 16.16 fixed point
    virtual std::array<int32_t, 3> query_weapon_world(uint8_t slot) = 0;
    /// Runs AimFrom for a slot and returns that piece's world position.
    ///
    /// @param slot weapon slot 0..2
    /// @return world position, 16.16 fixed point
    virtual std::array<int32_t, 3> aim_from_world(uint8_t slot) = 0;
    /// Calls the unit script's SetMaxReloadTime.
    ///
    /// @param milliseconds slowest reload of the unit's weapons
    virtual void set_max_reload_time(int32_t milliseconds) = 0;
};

struct SpawnCombatResult {
    bool resolved_nondefault_weapon{};
    InitializationResult initialization;
    // A host callback cleared a slot's definition: the slots after it are not
    // armed and the slowest reload is not reported.
    bool definition_cleared{};
};

/// Arms a new unit's three weapon slots and reports its slowest reload to its script.
///
/// Each slot loses its stockpile and takes the type's weapon, its slot index and
/// the enabled bit in the flags before the host is asked for its pieces; the
/// muzzle offset comes from the QueryWeapon and AimFrom pieces. SetMaxReloadTime
/// then gets the slowest reload in milliseconds. The reload countdown is the
/// canonical slot's, which the caller clears.
///
/// A host callback that clears a slot's definition ends the arming there.
///
/// @param[in,out] weapons unit's weapon slots
/// @param registry installed weapons
/// @param names FBI weapon1, weapon2 and weapon3
/// @param host script and model operations for the new unit
/// @return whether a weapon other than record zero resolved, the slowest reload, and
///         whether a callback cleared a definition
[[nodiscard]] SpawnCombatResult initialize_spawn_combat(
    UnitWeapons& weapons,
    const WeaponRegistry& registry,
    const std::array<std::string_view, weapon_slot_count>& names,
    SpawnGeometryHost& host
);

// A unit in the automatic target search. The pointer-sized identity is opaque:
// ownership and storage stay with the world.
using UnitIdentity = uintptr_t;
inline constexpr size_t player_mask_word_count = 2048;
inline constexpr uint32_t target_search_sample_limit = 50;
inline constexpr uint32_t source_range_check_bypass_flag = OA_UNIT_DEF_FLAG_KAMIKAZE;
// OA_UNIT_DEF_FLAG_SHOOT_ME in bits 8..15 of UnitDef.flags.
inline constexpr uint8_t candidate_type_auto_target_flag = 0x80U;
// weapon_paralyzer_flag in the low byte of WeaponDef.flags that
// TargetSource.weapon_flags holds.
inline constexpr uint8_t weapon_paralyzer_low_byte_flag =
    static_cast<uint8_t>(weapon_paralyzer_flag);
// Unit.state_flags bit the Paralyzed mission sets for as long as the unit is
// paralyzed (unit_health::paralyzed_state_flag). A paralyzer's automatic search
// passes over a unit carrying it.
inline constexpr uint8_t candidate_paralyzed_flag = 0x10U;

struct TargetUnit {
    UnitIdentity identity{};
    UnitIdentity owner{};
    std::array<uint32_t, 3> position{}; // signed 16.16 bit patterns
    uint32_t unit_flags{};              // Unit.flags
    uint8_t candidate_flags{};          // Unit.state_flags
    uint16_t category{};                // Unit.type_index, the bit category masks test
    uint8_t type_auto_target_flags{};   // bits 8..15 of UnitDef.flags
};

struct TargetSource : TargetUnit {
    uint8_t owner_spatial_index{};                         // Unit.owner_index
    bool owner_present{};                                  // Player.in_use != 0
    uint8_t owner_status{};                                // Player.status
    uint32_t type_flags{};                                 // UnitDef.flags
    std::array<uint8_t, weapon_slot_count> weapon_flags{}; // low byte of WeaponDef.flags
    std::array<std::span<const uint32_t>, weapon_slot_count> preferred_category_masks{};
    std::span<const uint32_t> implicit_excluded_category_mask; // UnitDef.no_chase_category
};

struct TargetSearchRequest {
    uint8_t weapon_slot{};
    bool explicit_radius{};
};

struct TargetSearchHost {
    virtual ~TargetSearchHost() = default;
    /// Returns how far the search looks around the source.
    ///
    /// @param source unit looking for a target
    /// @param request weapon slot and whether its own range is the radius
    /// @return radius in world units
    virtual int32_t
    search_radius(const TargetSource& source, const TargetSearchRequest& request) = 0;
    /// Collects the units the source's owner knows of near a point.
    ///
    /// @param owner_spatial_index owner's sighting-list index (Unit.owner_index)
    /// @param center search centre, signed 16.16 bit patterns
    /// @param radius world units
    /// @return candidates, valid until the next call; the search reorders
    ///     them as it draws
    virtual std::span<TargetUnit> gather_nearby(
        uint8_t owner_spatial_index, const std::array<uint32_t, 3>& center, int32_t radius
    ) = 0;
    /// Draws from the shared simulation random stream.
    ///
    /// @param exclusive_limit upper bound of the draw
    /// @return a value in [0, exclusive_limit)
    virtual uint32_t random_bounded(uint32_t exclusive_limit) = 0;
    /// Tests whether a slot's weapon can reach a candidate.
    ///
    /// @param source unit looking for a target
    /// @param target candidate
    /// @param slot weapon slot 0..2
    /// @return true when range, water, air and ballistic checks pass
    virtual bool
    weapon_can_reach(const TargetSource& source, const TargetUnit& target, uint8_t slot) = 0;
    /// Reports the console's shoot-all setting (OA_CONSOLE_FLAG_SHOOT_ALL in
    /// Game.console_flags).
    ///
    /// @return true when every unit may be targeted regardless of its type's flag
    virtual bool global_target_override() const noexcept = 0;
};

/// Chooses a target for one weapon slot among the units the owner knows of nearby.
///
/// Draws up to 50 random candidates, removing each drawn one by swapping in the
/// list's last entry, and drops those that are not targetable, excluded from
/// automatic targeting, out of reach, filtered by the type masks, or already
/// paralyzed when the slot's weapon is a paralyzer.
///
/// @param source unit looking for a target
/// @param request weapon slot 0..2 and whether its range is the radius
/// @param host sightings, random stream and reach test
/// @return the chosen unit, or 0 when none survives or the slot is out of range
/// @quirk Ranking uses a random draw bounded by the squared distance, and a unit
///        outside the slot's preferred-category mask wins over any unit inside it.
[[nodiscard]] UnitIdentity select_automatic_target(
    const TargetSource& source, const TargetSearchRequest& request, TargetSearchHost& host
);

// UnitDef.abilities / UnitDef.flags bits read by the strategic refresh.
inline constexpr uint32_t ability_can_attack = 0x00000010U;
inline constexpr uint32_t ability_can_load = 0x00000100U;
inline constexpr uint32_t def_flag_builder = 0x00000040U;
inline constexpr uint32_t def_flag_can_fly = 0x00000800U;
inline constexpr uint32_t def_flag_is_feature = 0x01000000U;

// Live-unit resolution behind a player's sighting lists.
struct IntelligenceHost {
    virtual ~IntelligenceHost() = default;
    /// Tests whether a unit slot holds a live, active unit.
    ///
    /// @param identity unit slot number from a sighting list
    /// @return true for a live, active unit
    virtual bool unit_active(UnitIdentity identity) = 0;
    /// Returns the target view of a unit.
    ///
    /// @param identity unit slot number from a sighting list
    /// @return the unit, or null when it cannot be resolved
    virtual const TargetUnit* resolve(UnitIdentity identity) = 0;
};

/// Appends the units of a player's sighting lists that lie within a radius.
///
/// The seen list is searched first; the radar list only when nothing was found
/// and the radar fallback is on.
///
/// @param seen unit slots the player currently sees
/// @param radar unit slots the player has on radar
/// @param radar_fallback whether the radar list may be searched
/// @param center search centre, signed 16.16 bit patterns; only X and Z are used
/// @param radius world units
/// @param host live-unit resolution
/// @param[in,out] found receives the units in range
/// @quirk Distances compare the high words of the squared 16.16 X and Z deltas
///        against the wrapped 32-bit square of the radius.
void gather_sightings(
    std::span<const uint16_t> seen,
    std::span<const uint16_t> radar,
    bool radar_fallback,
    const std::array<uint32_t, 3>& center,
    int32_t radius,
    IntelligenceHost& host,
    std::vector<TargetUnit>& found
);

struct StrategicType {
    uint32_t flags{};     // UnitDef.flags
    uint32_t abilities{}; // UnitDef.abilities
    uint8_t makes_metal{};
    int16_t min_water_depth{};
    int16_t radar_distance{};
    int16_t sonar_distance{};
    float build_cost_energy{};
    float build_cost_metal{};
    float energy_make{};
    float extracts_metal{};
    float energy_use{};
    float wind_generator{};
    float tidal_generator{};

    struct Weapon {
        uint16_t registry_index{};
        uint16_t damage_default{};
        int32_t range_world_units{};
    };

    std::array<Weapon, weapon_slot_count> weapons{};
};

struct StrategicRefreshState {
    std::vector<uint8_t> classifications; // the class of each unit type, by type index
    // Build priority, metal value and energy value of each type, each 0..100.
    std::vector<std::array<uint8_t, 3>> strengths;
    std::vector<int16_t> owned_counts; // the player's units of each type
};

struct StrategicRefreshContext {
    uint16_t pool_units_per_player{};
    uint16_t owner_unit_count{};
    int32_t wind_max{};
    int32_t wind_strength_divisor{};
    float wind_factor{};
    float tidal_strength{};
};

/// Computes the strategic class of a unit type for a computer player.
///
/// Starts at 11 for a metal extractor, else 1, plus 10 for a metal maker and 10
/// for a type whose energy_rate is negative; the metal cost times 0.01 and then
/// the energy cost times 0.002 are added and truncated in turn, then threat_score
/// as a signed byte.
///
/// @param type unit type fields the refresh reads
/// @param context wind and tidal factors of the map
/// @return the class, clamped to [-100, 100]
[[nodiscard]] int32_t
classify_type(const StrategicType& type, const StrategicRefreshContext& context) noexcept;

/// Rebuilds a computer player's per-type classes and build priorities.
///
/// For every type from index 1 on, stores the class and three strength bytes:
/// build priority, metal value and energy value, each 0..100.
///
/// @param[in,out] state classes and strengths are rebuilt; owned counts are read
/// @param types all unit types, indexed as in the game; index 0 is skipped
/// @param context owner's unit count and limit, and the map's wind and tidal values
/// @quirk Outputs are the low bytes of wrapped 32-bit values.
void strategic_refresh(
    StrategicRefreshState& state,
    std::span<const StrategicType> types,
    const StrategicRefreshContext& context
);

// The standing move and fire orders in Unit.flags (OA_UNIT_FLAG_MOVE_ORDER_MASK and
// OA_UNIT_FLAG_FIRE_ORDER_MASK), and the manoeuvre move order (1) in place there.
inline constexpr uint32_t standing_move_order_mask = 0x000c0000U;
inline constexpr uint32_t standing_fire_order_mask = 0x00300000U;
inline constexpr uint32_t standing_move_order_manoeuvre = 0x00040000U;
inline constexpr uint8_t move_order_kind = 2;
inline constexpr uint8_t attack_order_kind = 3;

struct AttackOrderRequest {
    uint8_t kind{}; // command byte from AttackHost::resolve_order
    UnitIdentity target{};
    std::array<uint32_t, 3> position{};
    bool has_position{};
    uint16_t leash_length{}; // maneuver leash of the source; 0 for a plain attack
    // Where the source stands, x and z in whole world units, on the attack order of an
    // unforced unit on the manoeuvre standing move order; 0 otherwise.
    int16_t source_x{};
    int16_t source_z{};
};

struct AttackSource {
    UnitIdentity identity{};
    uint32_t flags{}; // Unit.flags
    std::array<uint32_t, 3> position{};
    uint16_t maneuver_leash_length{}; // UnitDef.maneuver_leash_length
};

struct AttackHost {
    virtual ~AttackHost() = default;
    /// Resolves the command byte for an order the source would be given.
    ///
    /// @param requested_kind attack_order_kind or move_order_kind
    /// @param source unit receiving the order
    /// @param target unit attacked, or null for a move
    /// @param position destination of a move, or null for an attack
    /// @return the command byte; zero rejects an attack
    /// @quirk A zero for a move still builds an order carrying that zero byte.
    virtual uint8_t resolve_order(
        uint8_t requested_kind,
        const AttackSource& source,
        const TargetUnit* target,
        const std::array<uint32_t, 3>* position
    ) = 0;
    /// Allocates the orders with the game's order defaults and puts them at the head
    /// of the source's order list, all or none.
    ///
    /// @param source unit receiving the orders
    /// @param orders one attack, or a move followed by an attack
    /// @return false when they could not all be added; combat code then reports failure
    virtual bool
    commit_orders(const AttackSource& source, std::span<const AttackOrderRequest> orders) = 0;
};

/// Queues an attack on a unit at the head of the source's orders.
///
/// Unforced, a unit on standing move order 1 also queues a move back to where it
/// stands behind the attack, which then carries the leash and the start point.
///
/// @param source unit given the order
/// @param target unit to attack
/// @param forced true for an ordered attack that ignores the standing orders
/// @param host order resolution and allocation
/// @return true when the orders were added; false for the unit itself, for a zero
///         standing move or fire order unless forced, when no attack command
///         resolves, or when the host could not add them
[[nodiscard]] bool issue_attack_order(
    const AttackSource& source, const TargetUnit& target, bool forced, AttackHost& host
);

struct WeaponScoreInput {
    bool present = false;
    int32_t range_world_units = 0; // WeaponDef.range
    uint16_t default_damage = 0;   // WeaponDef.damage_default
};

/// Scores how dangerous a unit type is from its weapons.
///
/// @param can_attack whether the type can attack (OA_UNIT_DEF_ABILITY_CAN_ATTACK in
///        UnitDef.abilities)
/// @param weapons the type's three slots; each present one (a non-zero id) adds
///        range/100 + 5 + damage/40
/// @return 11 if the type can attack, else 1, plus the weapon terms, clamped to [-100, 100]
[[nodiscard]] int threat_score(bool can_attack, const WeaponScoreInput weapons[3]) noexcept;

/// Returns a unit type's energy use, with wind and tidal output counted as negative use.
///
/// @param energy_use UnitDef.energy_use
/// @param wind_generator UnitDef.wind_generator
/// @param tidal_generator UnitDef.tidal_generator
/// @param wind_factor map wind factor (Game.wind_factor)
/// @param tidal_factor map tidal strength (Game.tidal_strength)
/// @return `energy_use` when set, else `-wind_factor * wind_generator` or
///         `-tidal_factor * tidal_generator`, else zero
[[nodiscard]] float energy_rate(
    float energy_use,
    float wind_generator,
    float tidal_generator,
    float wind_factor,
    float tidal_factor
) noexcept;
} // namespace oa::sim::combat_state
