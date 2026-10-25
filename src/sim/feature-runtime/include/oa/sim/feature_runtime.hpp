// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// Live map features over the canonical World: the placed-feature record pool,
// placement and removal on MapPlot cells, burning, death/reclaim sequences,
// falling 3DO wrecks and tree reproduction.

#include "oa/core/world.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace oa::sim::feature_runtime {

// MapPlot.feature words at or above OA_PLOT_FEATURE_RESERVED name no FeatureDef.
inline constexpr uint16_t no_feature = 0xffff;
inline constexpr uint16_t feature_continuation =
    0xfffe; // footprint cell; feature_record holds z/x back offsets
inline constexpr uint16_t feature_marker = 0xfffc; // blocking TNT marker cell without a definition
inline constexpr uint16_t hidden_edge =
    0xfffd; // blocking plot under a map edge the view never shows

// Map edges the view never shows: Game.map_pixel_width/height are the world
// size less these (the "Edge" console command's defaults).
inline constexpr int32_t hidden_right_edge = 0x20;
inline constexpr int32_t hidden_bottom_edge = 0x80;

// Placed-feature pool: 0x18000 bytes of 0x30-byte records.
inline constexpr int32_t slot_capacity = 0x800;
inline constexpr int32_t no_slot = -1;

// MapPlot.flags bits 3..6 carry the placing player; 10 is "none".
inline constexpr uint8_t plot_player_shift = 3;
inline constexpr uint8_t no_player = 10;

// Bits of PlacedFeature.state.
inline constexpr uint8_t state_burning = 0x01;
inline constexpr uint8_t state_reclaimed = 0x02; // leaves featurereclamate instead of featuredead
inline constexpr uint8_t state_has_shadow = 0x04;
inline constexpr uint8_t state_no_spread =
    0x08; // mirrored fire (from another player's machine) does not spread
// Set with state_reclaimed when start_feature_sequence starts a reclamate
// sequence, cleared when it starts a die sequence; nothing reads it.
inline constexpr uint8_t state_reclaim_sequence = 0x10;

// A burning feature puffs smoke every third tick into effect layer 5.
inline constexpr uint32_t smoke_period = 3;
inline constexpr uint32_t burning_smoke_layer = 5;
// Effect layer of a geothermal vent's endless smoke.
inline constexpr uint32_t geothermal_smoke_layer = 4;
// Sinking speed of a 3DO wreck once below sea level (16.16 per tick).
inline constexpr oa_fixed wreck_sink_speed = static_cast<oa_fixed>(0xffffd334u);

// A feature change settled on this machine, for the other players.
enum class FeatureChange : uint8_t {
    ignited,     // caught fire here
    destroyed,   // weapon damage started its die sequence
    reclaimed,   // a unit finished reclaiming it
    resurrected, // a unit raised the wreck back into a unit, which removed it
};

#pragma pack(push, 1)

// GAF sequence cursor (the sprite-animation cursor layout).
struct FeatureCursor {
    uint16_t frame{};
    uint16_t remaining{}; // ticks left on this frame
    uint8_t repeat{};
    uint8_t unused_after_repeat[3]{}; // never written or read
    oa_ref32 sequence;                // FeatureDef seq_name_*; 0 once a one-shot sequence completes
};

// Object (3DO) feature: its model state and falling motion.
struct PlacedFeatureModel {
    oa_ref32 object; // model object state from create_object
    FixedVec3 position;
    FixedVec3 velocity;
};

// Sprite feature: the playing burn/die/reclaim sequence and its shadow.
struct PlacedFeatureSprite {
    FeatureCursor animation;
    FeatureCursor shadow;
    uint8_t unused_tail[4]{}; // pads this view to the 3DO view's size; never written or read
};

// One record of the placed-feature pool; MapPlot.feature_record indexes it
// while the plot has OA_PLOT_FLAG_ANIMATING_FEATURE.
struct PlacedFeature {
    /// Builds a cleared record, its union cleared through the 3DO view.
    ///
    /// Spelled out because some compilers delete the implicit constructor of
    /// a class whose anonymous union holds a member with initialisers.
    PlacedFeature() noexcept : model{} {}

    int16_t next{};
    int16_t prev{};

    union {
        PlacedFeatureModel model;
        PlacedFeatureSprite sprite;
    };

    int16_t orientation[3]{};
    uint16_t damage{};
    int16_t cell_x{};
    int16_t cell_z{};
    uint16_t def_index{}; // FeatureDef table index
    uint8_t spread_countdown{};
    uint8_t state{}; // state_*
};

#pragma pack(pop)

static_assert(sizeof(FeatureCursor) == 0xc);
static_assert(sizeof(PlacedFeature) == 0x30);
static_assert(std::is_trivially_copyable_v<PlacedFeature>);
static_assert(offsetof(PlacedFeature, model) == 0x4);
static_assert(offsetof(PlacedFeature, orientation) == 0x20);
static_assert(offsetof(PlacedFeature, damage) == 0x26);
static_assert(offsetof(PlacedFeature, cell_x) == 0x28);
static_assert(offsetof(PlacedFeature, def_index) == 0x2c);
static_assert(offsetof(PlacedFeature, spread_countdown) == 0x2e);
static_assert(offsetof(PlacedFeature, state) == 0x2f);

// The three intrusive lists threaded through the pool, whose heads are
// Game.feature_active_head, feature_settled_head and feature_free_head.
enum class FeatureList : uint8_t {
    active,  // animating, burning or falling
    settled, // 3DO wrecks at rest
    free_slots,
};

// One frame of a feature GAF sequence as the host resolves it.
struct FeatureSequenceFrame {
    uint16_t frame_count{};
    uint8_t repeat{};
    uint16_t duration{};
    int16_t width{};
    int16_t height{};
    int16_t origin_x{};
    int16_t origin_y{};
};

// Platform and cross-system calls made by the feature code. Null entries are
// skipped (random returns 0, sequence_frame reports no frame). sequence_frame
// fills frame_count and repeat even when the frame is past the end, and
// returns false then.
struct FeatureHost {
    void* context{};
    /// Draws from the shared deterministic stream.
    ///
    /// @param context FeatureHost::context
    /// @param limit exclusive upper bound
    /// @return a value in 0..limit-1
    uint32_t (*random)(void* context, uint32_t limit){};
    /// Draws from the match's linear congruential stream; used for smoke jitter only.
    ///
    /// @param context FeatureHost::context
    /// @return a value in 0..0x7fff
    int32_t (*lcg_random)(void* context){};
    /// Resolves one frame of a GAF sequence.
    ///
    /// @param context FeatureHost::context
    /// @param sequence sequence ref
    /// @param frame frame index
    /// @param[out] out frame count and repeat flag, always; duration and image geometry when the frame exists
    /// @return false when the frame is past the end
    bool (*sequence_frame)(
        void* context, oa_ref32 sequence, uint16_t frame, FeatureSequenceFrame* out
    ){};
    /// Creates the model state of a 3DO feature.
    ///
    /// @param context FeatureHost::context
    /// @param def the feature's definition
    /// @return the model object ref
    oa_ref32 (*create_object)(void* context, const FeatureDef* def){};
    /// Destroys a 3DO feature's model state.
    ///
    /// @param context FeatureHost::context
    /// @param object ref create_object returned
    void (*destroy_object)(void* context, oa_ref32 object){};
    /// Refreshes the movement maps over a changed feature footprint.
    ///
    /// @param context FeatureHost::context
    /// @param cell_x footprint origin column
    /// @param cell_z footprint origin row
    /// @param width footprint width in cells
    /// @param height footprint depth in cells
    void (*footprint_changed)(
        void* context, int16_t cell_x, int16_t cell_z, int16_t width, int16_t height
    ){};
    /// Starts a geothermal vent's endless smoke on an effect layer.
    ///
    /// @param context FeatureHost::context
    /// @param position signed 16.16 world position of the vent
    /// @param layer effect layer (geothermal_smoke_layer)
    void (*emit_feature_fx)(void* context, const FixedVec3* position, uint32_t layer){};
    /// Emits one light smoke puff on an effect layer.
    ///
    /// @param context FeatureHost::context
    /// @param position signed 16.16 world position of the puff
    /// @param layer effect layer (burning_smoke_layer)
    void (*emit_smoke)(void* context, const FixedVec3* position, uint32_t layer){};
    /// Plays a named sound at a world position.
    ///
    /// @param context FeatureHost::context
    /// @param name sound name, such as "treeburn"
    /// @param position signed 16.16 world position
    void (*play_sound)(void* context, const char* name, const FixedVec3* position){};
    /// Fires a feature's burn weapon, damaging what stands around the fire.
    ///
    /// @param context FeatureHost::context
    /// @param weapon WeaponDef ref of the feature's burnweapon
    /// @param position signed 16.16 world position of the fire
    void (*burn_weapon)(void* context, oa_ref32 weapon, const FixedVec3* position){};
    /// Credits the economy when a unit finishes reclaiming a feature.
    ///
    /// @param context FeatureHost::context
    /// @param unit the reclaiming unit
    /// @param energy the feature's energy
    /// @param metal the feature's metal
    void (*credit_reclaim)(void* context, Unit* unit, float energy, float metal){};
    /// Reports whether another player's simulation settles this weapon hit on a feature.
    ///
    /// @param context FeatureHost::context
    /// @param weapon_id the hitting weapon's id
    /// @param cell_x hit plot column
    /// @param cell_z hit plot row
    /// @return true when nothing is to be applied here
    bool (*feature_hit_elsewhere)(
        void* context, uint16_t weapon_id, int32_t cell_x, int32_t cell_z
    ){};
    /// Reports a feature change settled here to the other players.
    ///
    /// @param context FeatureHost::context
    /// @param change what happened
    /// @param cell_x feature cell column
    /// @param cell_z feature cell row
    /// @param reclaimer the unit that finished reclaiming the feature; null
    ///        for a fire or a destruction
    void (*feature_changed)(
        void* context, FeatureChange change, int32_t cell_x, int32_t cell_z, const Unit* reclaimer
    ){};
};

/// Views World.placed_features as placed-feature records.
///
/// @param world world holding the pool storage
/// @return the first record, or null without storage
[[nodiscard]] PlacedFeature* feature_records(World& world) noexcept;
/// Returns the placed-feature record in a pool slot.
///
/// @param world world holding the pool
/// @param slot record index
/// @return the record, or null for a slot outside the pool
[[nodiscard]] PlacedFeature* feature_record(World& world, int32_t slot) noexcept;
/// Returns the placed-feature record in a pool slot, to read.
///
/// @param world world holding the pool
/// @param slot record index
/// @return the record, or null for a slot outside the pool
[[nodiscard]] const PlacedFeature* feature_record(const World& world, int32_t slot) noexcept;
/// Returns the first slot of one of the pool's lists (Game.feature_active_head and the two after it).
///
/// @param world world holding the list heads
/// @param list list to read
/// @return the head slot, or no_slot for an empty list
[[nodiscard]] int32_t feature_list_head(const World& world, FeatureList list) noexcept;
/// Sets the first slot of one of the pool's lists.
///
/// @param[in,out] world world holding the list heads
/// @param list list to write
/// @param slot new head slot, or no_slot
void set_feature_list_head(World& world, FeatureList list, int32_t slot) noexcept;
/// Returns the plot index the reproduction pass visits next (Game.reproduce_cursor).
///
/// @param world world holding the cursor
/// @return the plot index
[[nodiscard]] int32_t reproduce_cursor(const World& world) noexcept;
/// Sets the plot index the reproduction pass visits next (Game.reproduce_cursor).
///
/// @param[in,out] world world holding the cursor
/// @param index plot index
void set_reproduce_cursor(World& world, int32_t index) noexcept;

/// Returns the height of the plot at a cell.
///
/// @param world world holding the plots
/// @param cell_x plot column
/// @param cell_z plot row
/// @return the plot height, 0 off the map
[[nodiscard]] uint8_t plot_height(const World& world, int16_t cell_x, int16_t cell_z) noexcept;
/// Samples the bilinear ground height under a 16.16 X/Z.
///
/// @param world world holding the plots
/// @param x signed 16.16 world X
/// @param z signed 16.16 world Z
/// @return the interpolated height, -1 outside the map interior
[[nodiscard]] int32_t sample_height(const World& world, oa_fixed x, oa_fixed z) noexcept;
/// Returns the mean of the plot's high and low heights under a 16.16 X/Z.
///
/// @param world world holding the plots
/// @param x signed 16.16 world X
/// @param z signed 16.16 world Z
/// @return (low + high) >> 1, -1 off the map
[[nodiscard]] int32_t mean_plot_height(const World& world, oa_fixed x, oa_fixed z) noexcept;
/// Returns the 16.16 centre of a feature footprint placed at a cell, on the ground.
///
/// @param world world holding the plots
/// @param cell_x footprint origin column
/// @param cell_z footprint origin row
/// @param def the feature's definition, for its footprint
/// @return the centre, Y from sample_height shifted left 16
[[nodiscard]] FixedVec3
feature_center(const World& world, int16_t cell_x, int16_t cell_z, const FeatureDef& def) noexcept;

/// Returns the height a feature is drawn at.
///
/// An object feature (a wreck) is drawn where its record stands, which
/// falls to the ground and sinks to the sea bed; a sprite feature is drawn
/// at the height of the plot holding its footprint's origin.
///
/// @param world world holding the plots and the feature records
/// @param cell_x footprint origin column
/// @param cell_z footprint origin row
/// @return the height, 16.16; 0 off the map
[[nodiscard]] oa_fixed
feature_drawn_height(const World& world, int32_t cell_x, int32_t cell_z) noexcept;

/// Links every record into the free list and empties the live lists.
///
/// @param[in,out] world world holding the pool
/// @return false when World.placed_features cannot hold slot_capacity records
bool init_feature_pool(World& world) noexcept;
/// Destroys the model state of every live 3DO feature.
///
/// @param world world holding the pool
/// @param host receives destroy_object for each settled and active 3DO record
void release_feature_objects(World& world, const FeatureHost& host) noexcept;
/// Pops a free record into the active list and clears its burning bit.
///
/// @param[in,out] world world holding the pool
/// @return the slot, or slot_capacity when none is free
[[nodiscard]] int32_t alloc_feature_slot(World& world) noexcept;
/// Unlinks a record from its list and pushes it onto the head of another.
///
/// @param[in,out] world world holding the pool
/// @param slot record to move; an invalid slot is ignored
/// @param to destination list
void move_feature_slot(World& world, int32_t slot, FeatureList to) noexcept;

/// Puts a feature on a plot across its footprint, clearing what was there.
///
/// A 3DO feature takes a pool record, positioned at `position` or the footprint
/// centre, and model state from the host. The origin plot records the placing
/// player in flags bits 3..6; the other footprint cells become continuations.
/// A geothermal feature starts its vent smoke. The footprint's movement maps are
/// refreshed. def_index feature_marker only writes the marker word.
///
/// @param[in,out] world world holding the plots and pool
/// @param host feature host
/// @param plot_index row-major index of the origin plot
/// @param def_index FeatureDef table index
/// @param position signed 16.16 model position, or null for the footprint centre
/// @param orientation three 16-bit angles, or null for zero
/// @param player placing player, or no_player
/// @return the new record for 3DO features; null for sprites, markers and on failure (footprint off the map, an indestructible overlap or no free record)
PlacedFeature* place_feature(
    World& world,
    const FeatureHost& host,
    std::size_t plot_index,
    uint16_t def_index,
    const FixedVec3* position,
    const int16_t* orientation,
    uint8_t player
) noexcept;
/// Removes the feature covering a plot, following a continuation cell.
///
/// Frees the feature's pool record and 3DO model state, clears the origin and
/// its continuation cells and refreshes the movement maps.
///
/// @param[in,out] world world holding the plots and pool
/// @param host feature host
/// @param plot_index row-major plot index
/// @param force also remove an indestructible feature
/// @return true when a feature was removed
bool clear_plot_feature(
    World& world, const FeatureHost& host, std::size_t plot_index, bool force
) noexcept;
/// Clears every feature on the map, indestructible ones included.
///
/// @param[in,out] world world holding the plots and pool
/// @param host feature host
void clear_all_features(World& world, const FeatureHost& host) noexcept;
/// Swaps a feature for its featuredead (or featurereclamate) remnant.
///
/// A 3DO remnant keeps the freed record's position and orientation.
///
/// @param[in,out] world world holding the plots and pool
/// @param host feature host
/// @param cell_x feature origin column
/// @param cell_z feature origin row
/// @param reclaimed leave featurereclamate instead of featuredead; a record marked state_reclaimed also does
void replace_with_remnant(
    World& world, const FeatureHost& host, int32_t cell_x, int32_t cell_z, bool reclaimed
) noexcept;
/// Starts the die (or reclamate) sequence of a sprite feature, or replaces it at once when it has none.
///
/// A continuation cell is followed back to the origin. A sprite already playing
/// a sequence is left alone.
///
/// @param[in,out] world world holding the plots and pool
/// @param host feature host
/// @param cell_x feature cell column
/// @param cell_z feature cell row
/// @param reclaimed play seqnamereclamate and mark the record state_reclaimed
void start_feature_sequence(
    World& world, const FeatureHost& host, int32_t cell_x, int32_t cell_z, bool reclaimed
) noexcept;
/// Sets a feature with a burn sequence burning.
///
/// Takes a pool record, starts the burn sequences, sets the spread countdown to
/// half the spark time plus a random amount below it, and plays "treeburn".
/// Both are in ticks, so the stock spark time of 150 gives 75 to 149.
///
/// @param[in,out] world world holding the plots and pool
/// @param host feature host
/// @param cell_x feature origin column
/// @param cell_z feature origin row
/// @param mirrored fire that started in another player's simulation: it does not spread and is not reported
/// @quirk The countdown is one byte and keeps the low 8 bits of the sum, so a
///        spark time above 257 ticks can wrap it short, or to 0, and a fire
///        whose countdown is 0 never spreads.
void ignite_feature(
    World& world, const FeatureHost& host, int32_t cell_x, int32_t cell_z, bool mirrored
) noexcept;
/// Lights the neighbours of a burnt-down fire and fires its burn weapon.
///
/// Each flammable feature within three cells, then along five steps downwind,
/// ignites when a random percentage falls below its spreadchance.
///
/// @param[in,out] world world holding the plots, pool and wind
/// @param host feature host
/// @param def the burning feature's definition
/// @param cell_x fire origin column
/// @param cell_z fire origin row
void spread_fire(
    World& world, const FeatureHost& host, const FeatureDef& def, int16_t cell_x, int16_t cell_z
) noexcept;
/// Replaces a burnt-out feature with its featureburnt remnant.
///
/// @param[in,out] world world holding the plots and pool
/// @param host feature host
/// @param record the burning feature's record
void burn_out_feature(World& world, const FeatureHost& host, const PlacedFeature& record) noexcept;
/// Returns a jittered smoke origin inside the current burn frame.
///
/// @param world world holding the plots
/// @param host feature host, for the frame geometry and the lcg_random smoke jitter
/// @param def the burning feature's definition
/// @param record the burning feature's record
/// @return signed 16.16 world position; the footprint centre when the frame is unknown
[[nodiscard]] FixedVec3 smoke_position(
    const World& world, const FeatureHost& host, const FeatureDef& def, const PlacedFeature& record
) noexcept;
/// Applies weapon damage to the feature on a plot.
///
/// Does nothing unless the tree-death console option is on, the feature is
/// destructible and no other player settles the hit. A fire-starting weapon
/// ignites a flammable feature. Otherwise damage accumulates in the plot's
/// record word, or in a 3DO record's damage, and at the feature's damage
/// the die sequence starts and the change is reported.
///
/// @param[in,out] world world holding the plots and pool
/// @param host feature host
/// @param plot_index row-major index of the hit plot
/// @param cell_x hit plot column
/// @param cell_z hit plot row
/// @param weapon the hitting weapon
void damage_feature(
    World& world,
    const FeatureHost& host,
    std::size_t plot_index,
    int32_t cell_x,
    int32_t cell_z,
    const WeaponDef& weapon
) noexcept;
/// Finishes a unit's reclaim of the feature under a 16.16 position.
///
/// Credits the feature's energy and metal, starts its reclamate sequence and
/// reports the change with the reclaiming unit.
///
/// @param[in,out] world world holding the plots and pool
/// @param host feature host
/// @param[in,out] unit the reclaiming unit
/// @param position signed 16.16 world position reclaimed
/// @return false when there is nothing to reclaim, including a sprite already playing a sequence
bool reclaim_feature(
    World& world, const FeatureHost& host, Unit& unit, const FixedVec3& position
) noexcept;
/// Runs the per-tick feature pass.
///
/// Steps every animating definition's cursors, lets one plot per tick (walking
/// the map backwards) reproduce its feature, then advances each active record:
/// 3DO wrecks fall, sink below sea level and settle on the ground; burning
/// sprites smoke every third tick, burn out or spread; other sprites finish
/// their sequence into their remnant.
///
/// @param[in,out] world world holding the plots, pool and feature table
/// @param host feature host
/// @quirk Reproduction's row divides the plot index by the map height, not the width.
void tick_features(World& world, const FeatureHost& host) noexcept;

// One map-placed feature from the mission script (0x88 bytes each).
struct FeaturePlacement {
    char name[0x80]{};
    int32_t x{};
    int32_t z{};
};

static_assert(sizeof(FeaturePlacement) == 0x88);

/// Returns the plot a mission placement puts its feature's origin on.
///
/// A 3DO feature is centred on the placement by its footprint; a sprite starts there.
///
/// @param def the placed feature's definition
/// @param placement the mission placement
/// @param[out] cell_x origin column
/// @param[out] cell_z origin row
void feature_placement_cell(
    const FeatureDef& def, const FeaturePlacement& placement, int32_t* cell_x, int32_t* cell_z
) noexcept;

/// Places mission features by name.
///
/// @param[in,out] world world holding the plots and pool
/// @param host feature host; its context is passed to find_feature
/// @param placements mission placements; empty names are skipped
/// @param count number of placements
/// @param find_feature returns a FeatureDef index for a name, loading it when needed, or no_feature
void apply_feature_placements(
    World& world,
    const FeatureHost& host,
    const FeaturePlacement* placements,
    int32_t count,
    uint16_t (*find_feature)(void* context, const char* name)
) noexcept;

/// Blocks the plots under the map edges the view never shows.
///
/// Sets Game.map_pixel_width/height to the world size less the hidden edges,
/// then turns empty and continuation plots under those edges into hidden_edge:
/// the two right columns, the top plots whose height lifts them above the map,
/// and the plot above each bottom plot drawn below the visible height; on a lava
/// world also every plot whose low height is at or below sea level. The map load
/// runs it after every feature is placed.
///
/// @param[in,out] world world whose Game map size and plots are updated
/// @param lava_world also hide plots at or below sea level
void void_hidden_edges(World& world, bool lava_world) noexcept;

} // namespace oa::sim::feature_runtime
