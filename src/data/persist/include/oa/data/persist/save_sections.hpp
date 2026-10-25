// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Savegame sections: each writer/reader moves one part of the canonical game
// state in or out of a HAPIBANK account. Systems outside this package (unit
// creation, orders, scripts, feature placement, campaign data) are reached
// through SaveHooks.
#pragma once

#include "oa/data/persist/hapibank.hpp"
#include "oa/data/persist/save_orders.hpp"

#include "oa/core/feature_def.h"
#include "oa/core/game_state.h"
#include "oa/core/map_plot.h"
#include "oa/core/unit.h"
#include "oa/core/unit_def.h"
#include "oa/core/weapon_def.h"
#include "oa/core/world.h"
#include "oa/sim/world_environment/meteor.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace oa::data::persist {

// The plots the save sections read are MapPlot records, byte for byte; these
// are the byte offsets of the MapPlot fields they touch.
inline constexpr std::size_t plot_bytes = sizeof(MapPlot);

namespace plot {
inline constexpr std::size_t height = offsetof(MapPlot, height), metal = offsetof(MapPlot, metal),
                             feature = offsetof(MapPlot, feature),
                             feature_record = offsetof(MapPlot, feature_record),
                             flags = offsetof(MapPlot, flags);
} // namespace plot

inline constexpr uint16_t plot_first_reserved_feature = 0xfffb; // 0xfffb..0xffff: no own feature
inline constexpr uint8_t plot_flag_animating_feature = 0x01;
inline constexpr uint8_t plot_flags_player_features = 0x78; // four per-cell feature bits

// FeatureDef.flags bit clear for object (3DO) features.
inline constexpr uint16_t feature_def_flag_sprite = 0x0001;

// Placed-feature record (0x30 bytes); offsets the saves read or restore.
inline constexpr std::size_t feature_record_bytes = 0x30;

// A sprite feature's record holds its animation frame and playing sequence;
// an object (3DO) feature's holds its position, whose Y word shares the
// sequence's place, and its orientation. Both hold the damage and the
// burning feature's spread countdown, the ticks left before its fire
// spreads.
namespace feature_record {
inline constexpr std::size_t frame = 0x04, position = 0x08, sequence = 0x0c, orientation = 0x20,
                             damage = 0x26, spread_countdown = 0x2e;
inline constexpr std::size_t position_bytes = 0xc;    // X, Y and Z, 16.16
inline constexpr std::size_t orientation_bytes = 0x6; // three 16-bit angles
} // namespace feature_record

// The Features account's blobs. "Feature Type Names" holds one name per
// FeatureDef, NUL-padded to type_name_bytes; each record of "Normal
// Features", "Animating Features" and "3D Features" opens with the cell's X
// and Z words and, at record_type, the type word, which indexes those names.
namespace feature_section {
inline constexpr uint32_t type_name_bytes = 0x80;
inline constexpr uint32_t normal_record_bytes = 8, animating_record_bytes = 10,
                          object_record_bytes = 0x1a;
inline constexpr std::size_t record_type = 0x04;
} // namespace feature_section

// Movement object bytes copied into the mobility blob.
inline constexpr std::size_t movement_saved_offset = 0x08, movement_saved_bytes = 0x22,
                             movement_flags = 0x2e;
inline constexpr std::size_t mobility_blob_bytes = 0x23;

// Unit snapshot record ("Units" account, one id-blob per unit).
inline constexpr int32_t unit_snapshot_version = 0x11;
inline constexpr uint32_t unit_record_bytes = 0xb8;
inline constexpr uint32_t unit_record_legacy_bytes = 0xb6;

namespace unit_record {
inline constexpr std::size_t type_name = 0x00, owner = 0x20, id = 0x21, order_count = 0x23,
                             has_movement = 0x27, position = 0x2b, bank = 0x37, heading = 0x39,
                             pitch = 0x3b, health = 0x3d, veteran_level = 0x3f, weapons = 0x41,
                             carrier_id = 0x89, linked_id = 0x8b, carrier_slot = 0x8d,
                             last_attacker_owner = 0x8e, extracted_metal = 0x8f, cell_x = 0x93,
                             cell_z = 0x95, sight_center_x = 0x97, sight_center_z = 0x99,
                             footprint_x = 0x9b, footprint_z = 0x9d, squad = 0x9f,
                             decloak_until_tick = 0xa3, build_remaining = 0xa7, damage_kind = 0xab,
                             health_percent = 0xac, previous_health_percent = 0xad, events = 0xae,
                             sight_band = 0xb0, damage_countdown = 0xb1, state_flags = 0xb2,
                             flags = 0xb4;
inline constexpr std::size_t type_name_bytes = 0x20;
inline constexpr std::size_t weapon_bytes = 0x18;
} // namespace unit_record

// One weapon slot of the unit record: the offset of each UnitWeapon field it
// carries.
namespace weapon_record {
inline constexpr std::size_t target_a = 0x00, target_b = 0x02, aim_ready = 0x04, weapon_id = 0x08,
                             muzzle_offset = 0x0c, reload = 0x10, aim_heading = 0x12,
                             aim_pitch = 0x14, stockpile = 0x16, flags = 0x17;
inline constexpr uint8_t saved_flags = 0x1f; // UnitWeapon.flags bits kept
} // namespace weapon_record

/// How many bits of a saved weapon id the unit restore writes back.
///
/// byte is 3.1c's read of the low byte. wide is 16 bits, under a profile
/// whose weapon-types is above 256.
enum class WeaponIdWidth : uint8_t { byte, wide };

// Unit.flags as the unit writer packs them: the low nibble of
// Unit.build_flags in bits 0..3, flags 0..11 in bits 4..15, flag 13 in bit 16
// and flags 14..25 from bit 20. The unit restore hands flags 0..1 to the
// unit's creation as its state and copies the others back; flag 12 and flags
// 26..31 are not kept.
namespace unit_record_flags {
inline constexpr uint32_t build_nibble = 0x0000000fu;
inline constexpr uint32_t low = 0x00000fffu, low_shift = 4;
inline constexpr uint32_t state = 0x00000003u; // flags 0..1, the creation state
inline constexpr uint32_t construction_dirty = 0x00002000u, construction_dirty_shift = 3;
inline constexpr uint32_t high = 0xffffc000u, high_shift = 6;
inline constexpr uint32_t high_kept = 0x03ffc000u; // the flags bits 20..31 hold
inline constexpr uint32_t restored = (low & ~state) | construction_dirty | high_kept;
} // namespace unit_record_flags

inline constexpr uint8_t unit_record_no_carrier = 0xff;

// Objects outside the World tables that SaveHooks::resolve maps to bytes.
enum class SaveRef : uint8_t {
    movement,      // Unit.movement -> movement object
    player_info,   // Player.info -> player setup record
    mission_rules, // campaign rule block (see summary)
};

struct SaveHooks {
    void* context{};
    void* (*resolve)(void* context, SaveRef kind, oa_ref32 ref) = nullptr;
    // Camera placement (clamping belongs to the view); null stores the fields.
    void (*set_camera)(void* context, int32_t x, int32_t z) = nullptr;

    // Units.
    void (*write_script)(void* context, Unit* unit, Bank* bank) = nullptr;
    // Visits the unit's primary queue, then its secondary queue.
    void (*visit_orders)(void* context, const Unit* unit, SavedOrderVisit visit, void* walk) =
        nullptr;
    void (*restore_unit)(void* context, uint16_t unit_id, Bank* bank) = nullptr;

    // Features.
    void (*load_feature_set)(void* context) = nullptr;
    int16_t (*find_or_load_feature)(void* context, const char* name) = nullptr;
    void (*link_feature_set)(void* context) = nullptr;
    // Places a feature type on a plot. An object feature also gets its saved
    // position and orientation, laid out as feature_record holds them; both
    // are null otherwise.
    void (*place_feature)(
        void* context,
        uint8_t* plot,
        uint16_t type,
        const uint8_t* position,
        const uint8_t* orientation
    ) = nullptr;
    void (*burn_feature)(void* context, uint16_t x, uint16_t z) = nullptr;
    void (*queue_feature_event)(void* context, uint16_t x, uint16_t z, int32_t kind) = nullptr;
};

// What the sections read and write: the World plus map and feature tables it
// does not own yet.
struct SaveContext {
    World* world{};
    uint8_t* plots{};           // Game.map_cells target: map_width * map_height plots
    uint8_t* mapping{};         // Game.sight_grid target
    uint8_t* feature_records{}; // placed-feature records indexed by plot::feature_record
    sim::world_environment::MeteorState* meteor{};
    const SaveHooks* hooks{};
    // Records feature_records holds; the Features section reads and writes
    // none at or past it.
    uint32_t feature_record_count{};
};

/// Returns the plot of a map cell.
///
/// @param save save context holding the plots and map size
/// @param x cell column
/// @param z cell row
/// @return the cell's plot_bytes-byte plot, or null outside the map or when
///     the context has no plots
uint8_t* save_plot_at(const SaveContext* save, int32_t x, int32_t z);

/// Writes the camera position to the "Camera" account.
///
/// Game.camera_x goes to "X Position" and Game.camera_y to "Z Position".
///
/// @param save save context holding the game state
/// @param[in,out] bank bank to write; the Camera account is left open
void save_write_camera(const SaveContext* save, Bank* bank);

/// Restores the camera position from the "Camera" account.
///
/// Missing fields keep the current position. The position goes through the
/// set_camera hook, which clamps it to the view, or into the game fields
/// when the hook is null.
///
/// @param[in,out] save save context whose camera is restored
/// @param[in,out] bank bank to read; the Camera account is created when absent
void save_read_camera(SaveContext* save, Bank* bank);

/// Writes the meteor strike state to the "Meteor" account.
///
/// Stores Enabled, Active, Next Strike Time, Time Strike Ends, Next Hit Time
/// (ticks) and the origin and target cells.
///
/// @param meteor meteor state to save
/// @param[in,out] bank bank to write; the Meteor account is left open
void save_write_meteor(const sim::world_environment::MeteorState* meteor, Bank* bank);

/// Restores the meteor strike state from the "Meteor" account.
///
/// Missing fields read as 0; the cells are truncated to 16 bits.
///
/// @param[in,out] meteor meteor state; only the saved fields change
/// @param[in,out] bank bank to read; the Meteor account is created when absent
void save_read_meteor(sim::world_environment::MeteorState* meteor, Bank* bank);

struct MeteorTdf {
    void* context{};
    bool (*text)(void* context, const char* key, char* out, std::size_t out_bytes) = nullptr;
    int32_t (*integer)(void* context, const char* key, int32_t fallback) = nullptr;
    double (*real)(void* context, const char* key, double fallback) = nullptr;
};
enum class MeteorConfigResult { loaded, missing, bogus };

/// Reads the [Default] meteor block of a map's configuration.
///
/// @param section accessors over the block; null when the block is missing
/// @param[out] out meteor settings: MeteorWeapon, MeteorRadius, MeteorDensity,
///     MeteorDuration and MeteorInterval; partly written on failure
/// @return missing without a block; bogus (the game's fatal report) when
///     MeteorWeapon is missing or any of the four numbers is 0; loaded otherwise
/// @quirk The interval is tested as the unrounded double, the others after
///     conversion to their fields.
MeteorConfigResult
load_meteor_config(const MeteorTdf* section, sim::world_environment::MeteorSettings* out);

/// Writes each map cell's metal byte, in row order, to the "Metal" account's "Plotmap" blob.
///
/// @param save save context holding the plots and map size
/// @param[in,out] bank bank to write; nothing is written when the buffer cannot be allocated
void save_write_metal_plotmap(const SaveContext* save, Bank* bank);

/// Restores each map cell's metal byte from the "Metal" account's "Plotmap" blob.
///
/// Nothing changes unless the account and blob already existed and the blob
/// holds exactly one byte per cell.
///
/// @param[in,out] save save context whose plots are updated
/// @param[in,out] bank bank to read
void save_read_metal_plotmap(SaveContext* save, Bank* bank);

/// Writes the four per-cell feature bits of the plot flags to the "PlayerFeatures" account's "Plotmap" blob.
///
/// Cells are packed in pairs, the even cell's bits in the high nibble and
/// the odd cell's in the low one; an odd last cell is dropped.
///
/// @param save save context holding the plots and map size
/// @param[in,out] bank bank to write
void save_write_player_features(const SaveContext* save, Bank* bank);

/// Restores the four per-cell feature bits of the plot flags from the "PlayerFeatures" account.
///
/// Nothing changes unless the account and blob already existed and the blob
/// holds one byte per pair of cells; the other flag bits are kept.
///
/// @param[in,out] save save context whose plots are updated
/// @param[in,out] bank bank to read
void save_read_player_features(SaveContext* save, Bank* bank);

/// Writes the terrain mapping grid (half a byte per cell) to the "Mapping" account's "Data" blob.
///
/// @param save save context holding the mapping grid; a null grid writes an empty blob
/// @param[in,out] bank bank to write
void save_write_terrain_mapping(const SaveContext* save, Bank* bank);

/// Restores the terrain mapping grid from the "Mapping" account's "Data" blob.
///
/// Nothing changes unless the account and blob already existed and the blob
/// holds exactly half a byte per cell.
///
/// @param[in,out] save save context whose mapping grid is restored
/// @param[in,out] bank bank to read
void save_read_terrain_mapping(SaveContext* save, Bank* bank);

/// Returns the height byte of a map cell.
///
/// @param save save context holding the plots
/// @param x cell column
/// @param z cell row
/// @return the plot's height byte, or 0 outside the map
uint8_t save_cell_height(const SaveContext* save, int16_t x, int16_t z);

/// Writes the placed features to the "Features" account.
///
/// "Feature Type Names" holds every FeatureDef name in 0x80-byte entries.
/// Each plot with a feature of its own is then appended, in row order, to
/// "3D Features" (object features: cell, type, damage, position and orientation),
/// "Normal Features" (cell, type, record index) or "Animating Features"
/// (cell, type, damage, the frame's low byte, then a byte packing the playing
/// sequence, 0 burn, 1 die or 2 reclamate, in its low nibble with the high
/// nibble of the spread countdown); an animating feature whose sequence is
/// not its burn, die or reclamate sequence is skipped, and so is an object or
/// animating feature whose record index is not below feature_record_count. A
/// plot whose feature word is not below the FeatureDef count writes nothing.
/// The three counts go to "Number of ... Features"; the feature type names go
/// to a blob of 0x80 bytes per type.
///
/// @param save save context holding the plots, feature records and FeatureDefs
/// @param[in,out] bank bank to write; nothing is written when the name buffer
///     cannot be allocated
void save_write_features(const SaveContext* save, Bank* bank);

/// Restores the placed features from the "Features" account.
///
/// Loads the feature set, maps each saved type name to the current FeatureDef
/// (same index, else a name search, else find_or_load_feature), links the set
/// and places the normal, animating and object features through the hooks;
/// animating features restart their burn, die or reclamate sequence, then
/// take their saved damage, frame and spread countdown. Records outside the
/// map or short are skipped; a saved type past the name table places type
/// 0xffff. Only the records below its count that a blob holds whole are
/// read, so a forged count can neither stall the load nor place a record
/// twice. After an animating or object record's placement and sequence
/// start, its plot must hold a feature of its own, and its record index,
/// which a normal record for the same cell may have set from the file, must
/// be below feature_record_count; otherwise the damage, frame and countdown
/// it carries are skipped. A placement refused over a plot the load hid
/// under the map's edges thus restores nothing.
///
/// @param[in,out] save save context whose plots and feature records are restored
/// @param[in,out] bank bank to read; the Features account is created when absent
/// @quirk A restored fire's spread countdown is the saved high nibble alone,
///     rounded down to a multiple of 16 ticks, so a fire saved with fewer
///     than 16 ticks left does not spread. A fire that started on another
///     player's machine, which never spreads, is restored as an ordinary
///     fire that can.
/// @quirk An animating record whose sequence does not start (a low nibble
///     past 2, or a type without the burn, die or reclamate sequence it
///     names) leaves its sprite placed without a record of its own, its plot's
///     record word 0. Its damage, frame and countdown still go to
///     placed-feature record 0, which may be another feature's.
void save_read_features(SaveContext* save, Bank* bank);

/// Writes a unit's energy then metal accumulator to its "u%04xacc" blob in the open account.
///
/// @param unit unit to save
/// @param[in,out] bank bank with the Units account open
void save_write_unit_economy(const Unit* unit, Bank* bank);

/// Restores a unit's energy then metal accumulator from its "u%04xacc" blob.
///
/// @param[in,out] unit unit to restore; a short blob overwrites only the bytes it holds
/// @param[in,out] bank bank with the Units account open
/// @return false when the blob is absent or short
bool save_read_unit_economy(Unit* unit, Bank* bank);

/// Writes a unit's movement object state to its "u%04xmob" blob in the open account.
///
/// The blob holds the movement_saved_bytes bytes at movement_saved_offset and
/// the low three bits of the flags byte at movement_flags (the other bits
/// carry no data).
///
/// @param movement the unit's movement object bytes
/// @param unit unit that owns it
/// @param[in,out] bank bank with the Units account open
void save_write_unit_mobility(const uint8_t* movement, const Unit* unit, Bank* bank);

/// Restores one weapon slot from its record as the unit restore does.
///
/// Targets, aim-ready word, muzzle offset, reload, aim angles,
/// stockpile and the kept flag bits (weapon_record::saved_flags) are restored.
/// The saved weapon id goes back into the slot's WeaponDef: width byte writes
/// the low byte only, and width wide writes both bytes.
///
/// @param[in,out] world world holding the slot's WeaponDef
/// @param slot weapon_record-laid-out bytes of the unit record
/// @param[in,out] weapon weapon slot to restore
/// @param width how many bits of the saved id are written
void save_restore_unit_weapon(
    World* world, const uint8_t* slot, UnitWeapon& weapon, WeaponIdWidth width
);

/// Fills one 0xb8-byte unit record.
///
/// @param save save context holding the world
/// @param unit unit to encode
/// @param order_count number of orders the unit's queues held
/// @param has_movement true when the unit's movement object, and so its
///     mobility blob, is saved
/// @param[out] record unit_record_bytes bytes, laid out as unit_record gives
void save_encode_unit_record(
    const SaveContext* save,
    const Unit* unit,
    int32_t order_count,
    bool has_movement,
    uint8_t* record
);

/// Writes every live unit to the "Units" account.
///
/// Per unit, in slot order: its script through the write_script hook into
/// "Script%i", each order of its queues as a "u%04xm%04x" blob (every order is
/// counted, written or not), its mobility blob when the movement object
/// resolves, its economy blob and its record in the id blob numbered by save
/// order. "Number of Units" and "Version" are written only when a unit was saved.
///
/// @param save save context holding the world and hooks
/// @param[in,out] bank bank to write; the Units account is left open
void save_write_units(const SaveContext* save, Bank* bank);

/// Lists the ids of the open account's id-addressed blobs below a count.
///
/// The unit walks visit these rather than every id below a count the save
/// supplies. They are copied out because restoring a unit opens blobs by
/// name, which can grow the blob table.
///
/// @param bank bank with an open account
/// @param count exclusive upper bound on the ids
/// @param[out] ids malloc'd ascending ids without repeats, freed by the
///     caller; null when there are none
/// @return number of ids
int32_t save_unit_record_ids(const Bank* bank, int32_t count, int32_t** ids);

/// Finds the record a unit's restore reads, for every unit id at once.
///
/// The walk reads the id blobs 0, 1, 2, ... of the "Units" account while each
/// id below "Number of Units" holds a blob, as the game's does, and stops at
/// the first blob that does not hold a whole unit_record_bytes record. A unit's
/// record is the first the walk reaches whose id field names it. A load that
/// looks its units up here reads each record once, where a walk for each unit
/// would read them once per unit.
///
/// @param[in,out] bank bank to read; the "Units" account is left open
/// @param[out] records the id blob holding the record of each unit id below
///     records.size(), indexed by unit id; -1 where the walk names none
void save_find_unit_records(Bank* bank, std::span<int32_t> records);

/// Restores the units of the "Units" account through the restore_unit hook.
///
/// Nothing happens unless the account existed with Version 0x11. Each id
/// blob below "Number of Units" that holds a whole record hands its unit id to
/// the hook; an older 0xb6-byte record is passed with id 0, which restores
/// nothing. Only ids that hold records are visited, so a forged count can
/// neither stall the load nor grow the bank.
///
/// @param[in,out] save save context holding the hooks
/// @param[in,out] bank bank to read
void save_read_units(SaveContext* save, Bank* bank);

// Image rows: width/height header then `height` rows of `width` bytes.
struct ImageRows {
    uint32_t width{};
    uint32_t height{};
    uint32_t stride{};
    const uint8_t* pixels{};
};

/// Writes an image into the open blob from its start: width and height as
/// 32-bit words, then `height` rows of `width` bytes.
///
/// @param image image to write
/// @param[in,out] bank bank with an open account and blob
void save_write_image_rows(const ImageRows* image, Bank* bank);

// Campaign, UI and mission data the Summary record needs.
struct SummaryHooks {
    void* context{};
    const char* build_date{};
    const char* build_time{};
    const char* (*campaign_name)(void* context) = nullptr;
    void (*advance_next_mission)(void* context) = nullptr;
    const char* (*mission_name)(void* context) = nullptr;
    int32_t (*game_type)(void* context) = nullptr;
    void (*bind_mission_info)(void* context) = nullptr;
    const ImageRows* (*radar_image)(void* context) = nullptr;
    void (*write_stats_panel)(void* context, Bank* bank) = nullptr;
    void (*save_conditions)(void* context, Bank* bank) = nullptr;
    /// Writes the ModProfile account of a game played under a mod's profile
    /// (save_profile.hpp), last; null, or a game without a profile, writes none.
    void (*save_profile)(void* context, Bank* bank) = nullptr;
};

inline constexpr int32_t game_mode_in_match = 6; // Game.mode for an in-match save
// Summary "Gametype" values.
inline constexpr int32_t game_type_campaign = 1;
inline constexpr int32_t game_type_skirmish = 2; // the Summary carries the skirmish rules

// Skirmish rule words of the settings block Game.skirmish_info points at.
namespace skirmish_rules {
inline constexpr std::size_t commander_death = 0x108, mapping = 0x10c, line_of_sight = 0x110,
                             line_of_sight_type = 0x114, location = 0x118;
} // namespace skirmish_rules

// Account, field and blob names shared by the savegame writers and readers.
namespace save_key {
inline constexpr const char* summary = "Summary";
inline constexpr const char* game_type = "Gametype";
inline constexpr const char* players = "Players"; // Summary count, and the Players account
inline constexpr const char* max_units = "maxunits";
inline constexpr const char* campaign = "Campaign";
inline constexpr const char* mission = "Mission";
inline constexpr const char* map = "Map";
inline constexpr const char* difficulty = "Difficulty";
inline constexpr const char* side = "Side";
inline constexpr const char* thumbs = "Thumbs";
inline constexpr const char* commander_death = "CommanderDeath";
inline constexpr const char* location = "Location";
inline constexpr const char* mapping = "Mapping";
inline constexpr const char* line_of_sight = "LineOfSight";
inline constexpr const char* line_of_sight_type = "LineOfSightType";
inline constexpr const char* between_missions = "BetweenMissions";
inline constexpr const char* description = "Description";
inline constexpr const char* game_id = "Game ID";
inline constexpr const char* game_time = "Game Time";
inline constexpr const char* radar_image = "Radar Image";
inline constexpr const char* human_player = "Human Player";
inline constexpr const char* player_format = "Player%i";
inline constexpr const char* controller = "Controller";
inline constexpr const char* units = "Units";
inline constexpr const char* unit_count = "Number of Units";
inline constexpr const char* script_format = "Script%i";
inline constexpr const char* order_format = "u%04xm%04x";
inline constexpr const char* economy_format = "u%04xacc";
inline constexpr const char* mobility_format = "u%04xmob";
inline constexpr const char* features = "Features";
inline constexpr const char* feature_type_names = "Feature Type Names";
inline constexpr const char* normal_features = "Normal Features";
inline constexpr const char* animating_features = "Animating Features";
inline constexpr const char* object_features = "3D Features";
inline constexpr const char* normal_feature_count = "Number of Normal Features";
inline constexpr const char* object_feature_count = "Number of 3D Features";
inline constexpr const char* animating_feature_count = "Number of Animating Features";
} // namespace save_key

// Room for a formatted account or blob name.
inline constexpr std::size_t save_name_bytes = 32;
// "Human Player" of a save that names none.
inline constexpr int32_t no_human_player = OA_PLAYER_COUNT;

/// Writes a complete savegame to a file.
///
/// The Summary account records the build date and time, unit limit,
/// campaign, mission and map, difficulty, the local player's side, player
/// count, game type, thumbs, the skirmish rules of a skirmish, the
/// between-missions mark outside a match, the description, game id and game
/// time. In a match (Game.mode game_mode_in_match) the radar image, camera,
/// stats panel, units, terrain mapping, features, player features, metal
/// plotmap, meteor state and campaign conditions follow, then, under a mod's
/// profile, its ModProfile account. The bank is written with
/// savegame_description and packed accounts.
///
/// @param[in,out] save save context holding the world and hooks
/// @param summary campaign, UI and mission hooks
/// @param path file path
/// @param description Summary description; null leaves it out
/// @param game_id Summary "Game ID"
/// @param files file boundary
/// @return false when the bank cannot be created or the file cannot be written
/// @quirk No account holds the projectiles in flight, so a loaded game starts
///     with none, as in 3.1c. A stockpiled missile in flight is lost with them:
///     its weapon's stockpile was spent at launch.
bool save_write_game(
    SaveContext* save,
    const SummaryHooks* summary,
    const char* path,
    const char* description,
    int32_t game_id,
    const FileSink* files
);

/// Saves the game under savegame/<token 1>.sav when a second command-line token exists.
///
/// The save gets command_line_description and command_line_game_id.
/// Creating the directory belongs to the file sink.
///
/// @param[in,out] save save context holding the world and hooks
/// @param summary campaign, UI and mission hooks
/// @param tokens command-line tokens
/// @param token_count number of tokens
/// @param files file boundary
/// @param[out] path_out receives the formatted path
/// @param path_bytes size of `path_out` in bytes
/// @return false with fewer than two tokens, a path that does not fit, or a failed save
bool save_command_line_game(
    SaveContext* save,
    const SummaryHooks* summary,
    const char* const* tokens,
    int32_t token_count,
    const FileSink* files,
    char* path_out,
    std::size_t path_bytes
);
inline constexpr int32_t command_line_game_id = 0x29a;
inline constexpr const char* command_line_description = "Generic Game Description";

} // namespace oa::data::persist
