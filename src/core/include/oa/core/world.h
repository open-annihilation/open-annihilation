// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

/* Native owner of the live game state. */
#ifndef OA_CORE_WORLD_H
#define OA_CORE_WORLD_H

#include "oa/core/feature_def.h"
#include "oa/core/game_state.h"
#include "oa/core/player_setup.h"
#include "oa/core/map_plot.h"
#include "oa/core/player.h"
#include "oa/core/projectile.h"
#include "oa/core/types.h"
#include "oa/core/unit.h"
#include "oa/core/unit_def.h"
#include "oa/core/weapon_def.h"

#include <stdlib.h>

OA_CORE_BEGIN

/* The game-state block plus the tables its references lead to. World itself
 * is a native struct (host pointers, no 3.1c layout); every record it owns is
 * canonical. world_alloc_tables allocates the unit, type, feature and
 * projectile tables, and weapon slots past the game state's table when the
 * capacity asks for them; the map tables (plots, sight grid, placed features)
 * belong to the map loader. Sides past the game state's table are bound by
 * the match, which owns them; the World only points at them. The accessors
 * never allocate. */
typedef struct World {
    Game game;
    Unit* units; /* unit_slot_count records; slot 0 is reserved */
    uint32_t unit_slot_count;
    UnitDef* unit_defs; /* unit_def_count records; index 0 is reserved */
    uint32_t unit_def_count;
    FeatureDef* feature_defs;
    uint32_t feature_def_count;
    /* Weapon slots from OA_WEAPON_DEF_COUNT on: record i is slot
     * OA_WEAPON_DEF_COUNT + i. NULL when the table holds 256. */
    WeaponDef* extra_weapon_defs;
    uint32_t extra_weapon_def_count;
    /* Sides from index OA_SIDE_COUNT on: record i is side OA_SIDE_COUNT + i.
     * Owned by whoever bound them (the match). The World never allocates
     * or frees them. */
    Side* extra_sides;
    uint32_t extra_side_count;
    Projectile* projectiles;                             /* OA_PROJECTILE_CAPACITY records */
    UnitEconomy player_economy[OA_PLAYER_COUNT];         /* targets of Player.economy */
    PlayerSetupInfo player_info[OA_PLAYER_RECORD_COUNT]; /* targets of Player.info */
    MapPlot* plots;           /* target of Game.map_cells: map_width * map_height */
    uint16_t* sight_grid;     /* target of Game.sight_grid: one word per cell */
    uint8_t* placed_features; /* OA_PLACED_FEATURE_BYTES records */
    uint32_t placed_feature_count;
    uint32_t environment_enabled; /* the map's waterdoesdamage setting */
    int32_t environment_damage;   /* the map's waterdamage: damage to units at or below sea level */
} World;

/* Table sizes for world_alloc_tables. */
typedef struct WorldCapacity {
    uint32_t unit_slots; /* including reserved slot 0 */
    uint32_t unit_defs;  /* including reserved index 0 */
    uint32_t feature_defs;
#ifdef __cplusplus
    /* Zero when a caller names only the three tables, so those initializers
     * stay warning-clean. C has no default and names the field. */
    uint32_t extra_weapon_defs = 0; /* weapon slots past OA_WEAPON_DEF_COUNT */
#else
    uint32_t extra_weapon_defs; /* weapon slots past OA_WEAPON_DEF_COUNT */
#endif
} WorldCapacity;

/// Allocates a zeroed World with no tables.
///
/// @return the new World, or NULL when allocation fails; release it with world_destroy
static inline World* world_create(void) {
    return (World*)calloc(1, sizeof(World));
}

/// Frees the unit, type, feature, projectile and extra weapon tables and zeroes their counts.
///
/// The map tables (plots, sight grid, placed features) belong to the map loader
/// and are left alone.
///
/// @param[in,out] world World whose tables are released
static inline void world_free_tables(World* world) {
    free(world->units);
    free(world->unit_defs);
    free(world->feature_defs);
    free(world->projectiles);
    free(world->extra_weapon_defs);
    world->units = NULL;
    world->unit_defs = NULL;
    world->feature_defs = NULL;
    world->projectiles = NULL;
    world->extra_weapon_defs = NULL;
    world->unit_slot_count = 0u;
    world->unit_def_count = 0u;
    world->feature_def_count = 0u;
    world->extra_weapon_def_count = 0u;
}

/// Frees a World created by world_create together with its tables.
///
/// @param world World to release; NULL is ignored
static inline void world_destroy(World* world) {
    if (world == NULL)
        return;
    world_free_tables(world);
    free(world);
}

/// Replaces the unit, type, feature, projectile and extra weapon tables with zeroed ones.
///
/// The projectile table always holds OA_PROJECTILE_CAPACITY records. On
/// failure the World is left with no tables.
///
/// @param[in,out] world World whose tables are replaced
/// @param capacity record counts, each including its reserved index 0
/// @return 1 on success, 0 when an allocation fails
static inline int world_alloc_tables(World* world, const WorldCapacity* capacity) {
    world_free_tables(world);
    world->units = (Unit*)calloc(capacity->unit_slots, sizeof(Unit));
    world->unit_defs = (UnitDef*)calloc(capacity->unit_defs, sizeof(UnitDef));
    world->feature_defs = (FeatureDef*)calloc(capacity->feature_defs, sizeof(FeatureDef));
    world->projectiles = (Projectile*)calloc(OA_PROJECTILE_CAPACITY, sizeof(Projectile));
    world->extra_weapon_defs =
        capacity->extra_weapon_defs != 0u
            ? (WeaponDef*)calloc(capacity->extra_weapon_defs, sizeof(WeaponDef))
            : NULL;
    if ((world->units == NULL && capacity->unit_slots != 0u) ||
        (world->unit_defs == NULL && capacity->unit_defs != 0u) ||
        (world->feature_defs == NULL && capacity->feature_defs != 0u) ||
        world->projectiles == NULL ||
        (world->extra_weapon_defs == NULL && capacity->extra_weapon_defs != 0u)) {
        world_free_tables(world);
        return 0;
    }
    world->unit_slot_count = capacity->unit_slots;
    world->unit_def_count = capacity->unit_defs;
    world->feature_def_count = capacity->feature_defs;
    world->extra_weapon_def_count = capacity->extra_weapon_defs;
    return 1;
}

/* oa_ref32 convention: a reference to element i of a World table stores i + 1;
 * 0 is null. Unit slot 0 is reserved and never referenced, so a unit slot
 * index maps to a reference with 0 kept as "none". */

/// Encodes a World table index as a reference.
///
/// @param index element index in its table
/// @return index + 1
static inline oa_ref32 oa_ref_from_index(uint32_t index) {
    return index + 1u;
}

/// Encodes a unit slot as a unit reference, keeping slot 0 as "none".
///
/// @param slot unit slot index; 0 means no unit
/// @return slot + 1, or 0 for slot 0
static inline oa_ref32 oa_unit_ref_from_slot(uint32_t slot) {
    return slot != 0u ? slot + 1u : 0u;
}

/// Decodes a unit reference to its slot index, keeping 0 as "none".
///
/// @param ref unit reference; 0 is null
/// @return ref - 1, or 0 for a null reference
static inline uint32_t oa_unit_slot_from_ref(oa_ref32 ref) {
    return ref != 0u ? ref - 1u : 0u;
}

/// Looks up a unit by slot index.
///
/// @param world World holding the unit table
/// @param slot unit slot index
/// @return the unit, or NULL when slot is past the table
static inline Unit* world_unit_at(World* world, uint32_t slot) {
    return slot < world->unit_slot_count ? &world->units[slot] : NULL;
}

/// Resolves a unit reference.
///
/// @param world World holding the unit table
/// @param ref unit reference; 0 is null
/// @return the unit, or NULL for a null or out-of-range reference
static inline Unit* world_unit(World* world, oa_ref32 ref) {
    return ref != 0u ? world_unit_at(world, ref - 1u) : NULL;
}

/// Returns the slot index of a unit record in the World's unit table.
///
/// @param world World holding the unit table
/// @param unit record inside world->units
/// @return the unit's slot index
static inline uint32_t world_unit_slot(const World* world, const Unit* unit) {
    return (uint32_t)(unit - world->units);
}

/// Encodes a unit record as a unit reference.
///
/// @param world World holding the unit table
/// @param unit record inside world->units, or NULL
/// @return the reference, or 0 for NULL
static inline oa_ref32 world_unit_ref(const World* world, const Unit* unit) {
    return unit != NULL ? world_unit_slot(world, unit) + 1u : 0u;
}

/// Resolves a unit-type reference.
///
/// @param world World holding the type table
/// @param ref type reference; 0 is null
/// @return the type, or NULL for a null or out-of-range reference
static inline UnitDef* world_unit_def(World* world, oa_ref32 ref) {
    return ref != 0u && ref <= world->unit_def_count ? &world->unit_defs[ref - 1u] : NULL;
}

/// Returns the type of a unit.
///
/// @param world World holding the type table
/// @param unit unit whose Unit.def is resolved
/// @return the type, or NULL for a null or out-of-range Unit.def
static inline UnitDef* world_unit_def_of(World* world, const Unit* unit) {
    return world_unit_def(world, unit->def);
}

/// Encodes a unit-type record as a reference.
///
/// @param world World holding the type table
/// @param def record inside world->unit_defs, or NULL
/// @return the reference, or 0 for NULL
static inline oa_ref32 world_unit_def_ref(const World* world, const UnitDef* def) {
    return def != NULL ? (oa_ref32)(def - world->unit_defs) + 1u : 0u;
}

/// Looks up one of the ten player slots.
///
/// @param world World holding the game state
/// @param index player index, 0..OA_PLAYER_COUNT-1
/// @return the player, or NULL for any other index
static inline Player* world_player(World* world, uint32_t index) {
    return index < OA_PLAYER_COUNT ? &world->game.players[index] : NULL;
}

/// Looks up a player record in the eleven-record table.
///
/// @param world World holding the game state
/// @param index player index; OA_PLAYER_COUNT selects Game.no_player
/// @return the record, or NULL past OA_PLAYER_COUNT
static inline Player* world_player_record(World* world, uint32_t index) {
    if (index < OA_PLAYER_COUNT)
        return &world->game.players[index];
    return index == OA_PLAYER_COUNT ? &world->game.no_player : NULL;
}

/// Resolves a player reference to one of the ten player slots.
///
/// @param world World holding the game state
/// @param ref player reference; 0 is null
/// @return the player, or NULL for a null or out-of-range reference
static inline Player* world_player_ref(World* world, oa_ref32 ref) {
    return ref != 0u ? world_player(world, ref - 1u) : NULL;
}

/// Encodes a player slot as a player reference.
///
/// @param world World holding the game state
/// @param player one of world->game.players, or NULL
/// @return the reference, or 0 for NULL
static inline oa_ref32 world_player_ref_of(const World* world, const Player* player) {
    return player != NULL ? (oa_ref32)(player - world->game.players) + 1u : 0u;
}

/// Returns the player that owns a unit.
///
/// @param world World holding the game state
/// @param unit unit whose Unit.owner is resolved
/// @return the owner, or NULL when the reference is null or out of range
static inline Player* world_unit_owner(World* world, const Unit* unit) {
    return world_player_ref(world, unit->owner);
}

/// Returns the player's inclusive range of unit slots.
///
/// @param world World holding the unit table
/// @param player player whose first_unit..last_unit range is read
/// @param[out] count number of units in the range; 0 when it is empty or unresolved
/// @return the first unit of the range, or NULL when count is 0
static inline Unit* world_player_units(World* world, const Player* player, uint32_t* count) {
    Unit* first = world_unit(world, player->first_unit);
    Unit* last = world_unit(world, player->last_unit);
    *count = first != NULL && last != NULL && last >= first ? (uint32_t)(last - first) + 1u : 0u;
    return *count != 0u ? first : NULL;
}

/// Resolves a weapon-type reference.
///
/// Refs 1 to OA_WEAPON_DEF_COUNT are the game state's inline table. Later refs
/// are World.extra_weapon_defs when that table exists. Ref 0 is null.
///
/// @param world World holding the weapon tables
/// @param ref weapon reference; 0 is null
/// @return the weapon type, or NULL for a null or out-of-range reference
static inline WeaponDef* world_weapon_def(World* world, oa_ref32 ref) {
    uint32_t extra;
    if (ref == 0u)
        return NULL;
    if (ref <= OA_WEAPON_DEF_COUNT)
        return &world->game.weapon_defs[ref - 1u];
    if (world->extra_weapon_defs == NULL)
        return NULL;
    extra = ref - OA_WEAPON_DEF_COUNT;
    if (extra > world->extra_weapon_def_count)
        return NULL;
    return &world->extra_weapon_defs[extra - 1u];
}

/// Encodes a weapon record as a reference.
///
/// A record in the game state's table is its slot plus one. A record in
/// extra_weapon_defs is OA_WEAPON_DEF_COUNT plus its index plus one.
///
/// @param world World holding the weapon tables
/// @param def record inside either table, or NULL
/// @return the reference, or 0 for NULL or a record in neither table
static inline oa_ref32 world_weapon_ref(const World* world, const WeaponDef* def) {
    uintptr_t address;
    uintptr_t first;
    uintptr_t past;
    uintptr_t index;
    if (def == NULL)
        return 0u;
    address = (uintptr_t)def;
    first = (uintptr_t)world->game.weapon_defs;
    past = first + (uintptr_t)OA_WEAPON_DEF_COUNT * sizeof(WeaponDef);
    if (address >= first && address < past) {
        index = (address - first) / sizeof(WeaponDef);
        if (address == first + index * sizeof(WeaponDef))
            return (oa_ref32)(index + 1u);
        return 0u;
    }
    if (world->extra_weapon_defs != NULL && world->extra_weapon_def_count != 0u) {
        first = (uintptr_t)world->extra_weapon_defs;
        past = first + (uintptr_t)world->extra_weapon_def_count * sizeof(WeaponDef);
        if (address >= first && address < past) {
            index = (address - first) / sizeof(WeaponDef);
            if (address == first + index * sizeof(WeaponDef))
                return (oa_ref32)(OA_WEAPON_DEF_COUNT + index + 1u);
        }
    }
    return 0u;
}

/// Returns how many weapon slots the World can resolve.
///
/// @param world World holding the weapon tables
/// @return OA_WEAPON_DEF_COUNT plus the extra table's record count
static inline uint32_t world_weapon_def_count(const World* world) {
    return OA_WEAPON_DEF_COUNT + world->extra_weapon_def_count;
}

/// Returns the side record at an index.
///
/// An index under OA_SIDE_COUNT is the game state's table, including a
/// zeroed record past side_count. A later index is extra_sides when the
/// index is a loaded side and that record was bound.
///
/// @param world World holding the side tables
/// @param index side index
/// @return the side, or NULL when a side past the game state's table is not bound
static inline Side* world_side(World* world, uint32_t index) {
    uint32_t extra;
    if (index < OA_SIDE_COUNT)
        return &world->game.sides[index];
    if (world->extra_sides == NULL || index >= world->game.side_count)
        return NULL;
    extra = index - OA_SIDE_COUNT;
    if (extra >= world->extra_side_count)
        return NULL;
    return &world->extra_sides[extra];
}

/// Returns how many sides the game loaded.
///
/// @param world World holding the side tables
/// @return game.side_count
static inline uint32_t world_side_count(const World* world) {
    return world->game.side_count;
}

/// Resolves a feature-type reference.
///
/// @param world World holding the feature table
/// @param ref feature reference; 0 is null
/// @return the feature type, or NULL for a null or out-of-range reference
static inline FeatureDef* world_feature_def(World* world, oa_ref32 ref) {
    return ref != 0u && ref <= world->feature_def_count ? &world->feature_defs[ref - 1u] : NULL;
}

/// Resolves a projectile reference.
///
/// @param world World holding the projectile pool
/// @param ref projectile reference; 0 is null
/// @return the projectile, or NULL before the pool exists or past OA_PROJECTILE_CAPACITY
static inline Projectile* world_projectile(World* world, oa_ref32 ref) {
    return world->projectiles != NULL && ref != 0u && ref <= OA_PROJECTILE_CAPACITY
               ? &world->projectiles[ref - 1u]
               : NULL;
}

/// Returns the settings block that Player.info refers to.
///
/// @param world World holding the settings blocks
/// @param player player whose info reference is resolved
/// @return the block, or NULL for a null or out-of-range reference
static inline PlayerSetupInfo* world_player_info(World* world, const Player* player) {
    return player->info != 0u && player->info <= OA_PLAYER_RECORD_COUNT
               ? &world->player_info[player->info - 1u]
               : NULL;
}

/// Returns the terrain plot at a map cell.
///
/// @param world World holding the plot table
/// @param x cell column, 0..map_width-1
/// @param z cell row, 0..map_height-1
/// @return the plot, or NULL outside the map or before the plots exist
static inline MapPlot* world_plot(World* world, int32_t x, int32_t z) {
    if (world->plots == NULL || x < 0 || z < 0 || x >= world->game.map_width ||
        z >= world->game.map_height)
        return NULL;
    return &world->plots[(size_t)z * (size_t)world->game.map_width + (size_t)x];
}

/// Returns the economy staging block that Player.economy refers to.
///
/// @param world World holding the economy blocks
/// @param player player whose economy reference is resolved
/// @return the block, or NULL for a null or out-of-range reference
static inline UnitEconomy* world_player_economy(World* world, const Player* player) {
    return player->economy != 0u && player->economy <= OA_PLAYER_COUNT
               ? &world->player_economy[player->economy - 1u]
               : NULL;
}

OA_CORE_END

#ifdef __cplusplus
namespace oa {
/// Const overload of world_unit_at.
inline const Unit* world_unit_at(const World* world, uint32_t slot) {
    return world_unit_at(const_cast<World*>(world), slot);
}

/// Const overload of world_unit.
inline const Unit* world_unit(const World* world, oa_ref32 ref) {
    return world_unit(const_cast<World*>(world), ref);
}

/// Const overload of world_unit_def.
inline const UnitDef* world_unit_def(const World* world, oa_ref32 ref) {
    return world_unit_def(const_cast<World*>(world), ref);
}

/// Const overload of world_unit_def_of.
inline const UnitDef* world_unit_def_of(const World* world, const Unit* unit) {
    return world_unit_def(world, unit->def);
}

/// Const overload of world_player.
inline const Player* world_player(const World* world, uint32_t index) {
    return world_player(const_cast<World*>(world), index);
}

/// Const overload of world_player_record.
inline const Player* world_player_record(const World* world, uint32_t index) {
    return world_player_record(const_cast<World*>(world), index);
}

/// Const overload of world_player_ref.
inline const Player* world_player_ref(const World* world, oa_ref32 ref) {
    return world_player_ref(const_cast<World*>(world), ref);
}

/// Const overload of world_unit_owner.
inline const Player* world_unit_owner(const World* world, const Unit* unit) {
    return world_player_ref(world, unit->owner);
}

/// Const overload of world_weapon_def.
inline const WeaponDef* world_weapon_def(const World* world, oa_ref32 ref) {
    return world_weapon_def(const_cast<World*>(world), ref);
}

/// Const overload of world_side.
inline const Side* world_side(const World* world, uint32_t index) {
    return world_side(const_cast<World*>(world), index);
}

/// Const overload of world_player_info.
inline const PlayerSetupInfo* world_player_info(const World* world, const Player* player) {
    return world_player_info(const_cast<World*>(world), player);
}

/// Const overload of world_plot.
inline const MapPlot* world_plot(const World* world, int32_t x, int32_t z) {
    return world_plot(const_cast<World*>(world), x, z);
}
} // namespace oa
#endif

#endif
