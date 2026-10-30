// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// Owner-authoritative unit replication over the canonical World. Each tick
// every machine packs its own players' units into one 0x2c record per player;
// receivers overwrite their copies of the sender's units from it and apply
// event records (create, damage, kill, fire, script start, ...) on arrival.
// Simulation behaviour the receiver triggers is reached through
// ReplicationSim; nothing here allocates.

#include "oa/core/world.h"
#include "oa/netgame/replication/deltas.hpp"
#include "oa/netgame/records.hpp"

namespace oa::netgame {

// Bits of MovementRecord.flags.
inline constexpr uint8_t movement_substate_mask = 0x03; // carried by the air delta
inline constexpr uint8_t movement_blocked = 0x04; // collision bit, carried by the ground delta

// Bits of GroundDriver.flags.
inline constexpr uint8_t ground_driver_path_set = 0x01;
inline constexpr uint8_t ground_driver_sent_blocked = 0x04; // movement_blocked as last sent
inline constexpr uint8_t ground_driver_resend = 0x08;
inline constexpr std::size_t ground_driver_path_capacity = 20;
inline constexpr std::size_t ground_delta_max_points = 3;

// Bits of AirDriver.flags.
inline constexpr uint8_t air_driver_resend = 0x01;
inline constexpr uint8_t air_driver_sent_substate_shift = 1; // bits 1..2
inline constexpr uint8_t air_driver_sent_substate_mask = 0x06;

// Kind reported by an air goal object.
inline constexpr uint8_t air_goal_kind_none = 0; // no goal object
inline constexpr uint8_t air_goal_kind_target = 2;
inline constexpr uint8_t air_goal_kind_seek = 3;

// Ground driver fields the stream reads or writes.
struct GroundDriver {
    uint8_t flags{};      // ground_driver_*
    int32_t path_count{}; // at most 20 on a local driver, 3 on a remote one
    int16_t path[ground_driver_path_capacity][2]{};
};

// Air driver fields the stream reads or writes.
struct AirDriver {
    uint8_t flags{};     // air_driver_*
    uint8_t goal_kind{}; // air_goal_kind_*; other kinds are not replicated
    AirTargetGoal target{};
    AirSeekGoal seek{};
};

// The part of a unit's movement record (target of Unit.movement) that
// replication touches. The locomotion owner keeps these in its side table.
struct MovementRecord {
    MovementClass driver{}; // chosen at creation from the def's can-fly flag
    bool remote_driver{};   // receiving class (owner status remote)
    uint8_t flags{};        // movement_substate_mask and movement_blocked
    uint32_t speed{};       // carried by the full unit record
    GroundDriver ground{};
    AirDriver air{};
};

// Simulation entry points the receiver calls. Any null hook is skipped,
// except where a documented fallback applies. Unit pointers are World units;
// every hook receives the struct's context and the receiving World.
struct ReplicationSim {
    void* context{};

    /// Looks up the movement record of a unit (the side table behind Unit.movement).
    ///
    /// @param context ReplicationSim.context.
    /// @param world Receiving world.
    /// @param unit Unit whose record is wanted; its Unit.movement is nonzero.
    /// @return The record, or null when the unit has none.
    MovementRecord* (*movement)(void* context, World* world, Unit* unit){};

    /// Handles a 0x09 unit-created record, or the one the receiver synthesises when a unit's def index changed.
    ///
    /// @param context ReplicationSim.context.
    /// @param world Receiving world.
    /// @param owner_index Player slot of the sender, which owns the unit.
    /// @param record The record; its unit index is the unit's slot.
    void (*create_unit)(
        void* context, World* world, uint8_t owner_index, const UnitCreatedRecord& record
    ){};

    /// Handles a 0x0a unit-link record, also used when a full record attaches or detaches a unit.
    ///
    /// @param context ReplicationSim.context.
    /// @param world Receiving world.
    /// @param record The link; linked_unit_index 0 detaches.
    void (*link_unit)(void* context, World* world, const UnitLinkRecord& record){};

    /// Handles a 0x0b unit-damage record.
    ///
    /// @param context ReplicationSim.context.
    /// @param world Receiving world.
    /// @param record The damage event.
    void (*apply_damage)(void* context, World* world, const UnitDamageRecord& record){};

    /// Handles a 0x0c unit-killed record (the variant for units owned elsewhere).
    ///
    /// @param context ReplicationSim.context.
    /// @param world Receiving world.
    /// @param record The kill event.
    void (*apply_kill)(void* context, World* world, const UnitKilledRecord& record){};

    /// Handles a 0x0d or 0xe0 weapon-fire record by launching the shot here as well.
    ///
    /// A narrow record arrives already widened. The weapon id is 16 bits.
    ///
    /// @param context ReplicationSim.context.
    /// @param world Receiving world.
    /// @param sender Player record of the sender, or null.
    /// @param record The shot.
    void (*weapon_fire)(
        void* context, World* world, Player* sender, const WideWeaponFireRecord& record
    ){};

    /// Handles a 0x0e or 0xe1 record: the first shot aimed at the record's point with its weapon blows up.
    ///
    /// A narrow record arrives already widened. The weapon id is 16 bits.
    ///
    /// @param context ReplicationSim.context.
    /// @param world Receiving world.
    /// @param record Target point and weapon id of the intercepted shot.
    void (*detonate_projectile)(
        void* context, World* world, const WideProjectileInterceptedRecord& record
    ){};

    /// Handles a 0x0f feature event with action 0xfd (flagged false) or 0xff (flagged true).
    ///
    /// @param context ReplicationSim.context.
    /// @param world Receiving world.
    /// @param x Plot column.
    /// @param y Plot row.
    /// @param flagged True for action 0xff.
    void (*queue_feature_event)(
        void* context, World* world, uint16_t x, uint16_t y, bool flagged
    ){};

    /// Handles a 0x0f feature event with action 0xfe: the feature on the plot catches fire.
    ///
    /// @param context ReplicationSim.context.
    /// @param world Receiving world.
    /// @param x Plot column.
    /// @param y Plot row.
    void (*tree_burn)(void* context, World* world, uint16_t x, uint16_t y){};

    /// Handles a feature hit: that weapon damages the feature on the plot.
    ///
    /// A narrow 0x0f whose action is not 0xfd, 0xfe or 0xff arrives as a weapon
    /// hit whose id is that action byte. A wide 0xe2 weapon hit carries the
    /// 16-bit id.
    ///
    /// @param context ReplicationSim.context.
    /// @param world Receiving world.
    /// @param weapon Weapon id.
    /// @param x Plot column.
    /// @param y Plot row.
    void (*feature_event)(void* context, World* world, uint16_t weapon, uint16_t x, uint16_t y){};

    /// Handles a 0x10 script start on a live unit.
    ///
    /// @param context ReplicationSim.context.
    /// @param world Receiving world.
    /// @param unit The live unit named by the record.
    /// @param record Script function index, argument count and the four locals.
    void (*cob_start)(void* context, World* world, Unit* unit, const CobStartRecord& record){};

    /// Sets or clears unit state flags for a 0x11 record or a full unit record.
    ///
    /// Fallback when null: state_flags |= mask (on) or &= ~mask (off).
    ///
    /// @param context ReplicationSim.context.
    /// @param world Receiving world.
    /// @param unit Unit to change.
    /// @param mask Flags to change.
    /// @param on True to set, false to clear.
    void (*toggle_state_flags)(void* context, World* world, Unit* unit, uint8_t mask, bool on){};

    /// Handles a 0x12 builder-link record.
    ///
    /// @param context ReplicationSim.context.
    /// @param world Receiving world.
    /// @param source The building unit, or null for unit index 0.
    /// @param subject The unit being built, or null for unit index 0.
    void (*link_builder)(void* context, World* world, Unit* source, Unit* subject){};

    /// Handles a 0x13 sound record: a nonzero indexed byte plays the sound by
    /// its index, else at its position.
    ///
    /// @param context ReplicationSim.context.
    /// @param world Receiving world.
    /// @param record Sound table index and position.
    void (*play_sound)(void* context, World* world, const SoundRecord& record){};

    /// Handles a 0x14 unit transfer of a live unit whose new owner plays on this machine (local or computer).
    ///
    /// @param context ReplicationSim.context.
    /// @param world Receiving world.
    /// @param unit The unit changing owner.
    /// @param new_owner Player record of the new owner.
    /// @param record The transfer, with the unit's carried state.
    void (*transfer_unit)(
        void* context, World* world, Unit* unit, Player* new_owner, const UnitTransferRecord& record
    ){};

    /// Sees a full unit record as it is read, before it changes the unit.
    ///
    /// Null sees nothing.
    ///
    /// @param context ReplicationSim.context.
    /// @param world Receiving world.
    /// @param unit The unit the record is for.
    /// @param record The record; a unit_def_index of 0 says the slot is empty.
    void (*full_record_read)(
        void* context, World* world, Unit* unit, const FullUnitRecord& record
    ){};

    /// Moves a unit to the position a detached full unit record carries.
    ///
    /// Fallback when null: position = to.
    ///
    /// @param context ReplicationSim.context.
    /// @param world Receiving world.
    /// @param unit Unit to place.
    /// @param to New position, 16.16 fixed-point x, y, z.
    /// @param occupancy The record's two occupancy bits.
    void (*place_unit)(
        void* context, World* world, Unit* unit, const FixedVec3& to, uint8_t occupancy
    ){};

    /// Stores a ground delta in the unit's remote navigator, after the movement record's own copy is updated.
    ///
    /// @param context ReplicationSim.context.
    /// @param world Receiving world.
    /// @param unit Unit the delta is for.
    /// @param delta Blocked bit and up to three route points.
    void (*apply_ground_delta)(
        void* context, World* world, Unit* unit, const WaypointDelta& delta
    ){};

    /// Sets the movement speed (MovementRecord.speed) a full record carries, after the record's own copy.
    ///
    /// @param context ReplicationSim.context.
    /// @param world Receiving world.
    /// @param unit Unit the record is for.
    /// @param speed Movement speed word as sent.
    void (*set_movement_speed)(void* context, World* world, Unit* unit, uint32_t speed){};

    /// Replaces the goal of a unit's remote air driver with the one an air delta carries.
    ///
    /// Called after the movement record's own copy is updated and before the
    /// substate; goal tags 0 and 3 leave no goal.
    ///
    /// @param context ReplicationSim.context.
    /// @param world Receiving world.
    /// @param unit Unit the delta is for.
    /// @param delta The air delta.
    void (*set_air_goal)(void* context, World* world, Unit* unit, const AirDelta& delta){};

    /// Applies the movement substate an air delta carries.
    ///
    /// Fallback when null: the movement flags' low two bits = substate.
    ///
    /// @param context ReplicationSim.context.
    /// @param world Receiving world.
    /// @param unit Unit the delta is for.
    /// @param substate Movement substate, 0..3.
    void (*set_movement_substate)(void* context, World* world, Unit* unit, uint8_t substate){};

    /// Steps the movement of every live mobile sender unit once after the delta list.
    ///
    /// @param context ReplicationSim.context.
    /// @param world Receiving world.
    /// @param unit Unit to step.
    void (*movement_tick)(void* context, World* world, Unit* unit){};

    /// Refreshes a sender unit's height and sight after its movement step.
    ///
    /// @param context ReplicationSim.context.
    /// @param world Receiving world.
    /// @param unit Unit to refresh.
    void (*sight_update)(void* context, World* world, Unit* unit){};

    /// Clears a word of the object the first reference in
    /// Unit.links_after_script names, before a full record stores its fields,
    /// as 3.1c does; what the word holds is not known.
    ///
    /// @param context ReplicationSim.context.
    /// @param world Receiving world.
    /// @param unit Unit the full record is for.
    void (*clear_linked_object_word)(void* context, World* world, Unit* unit){};
};

// ---- sender ----

/// Tells whether a local ground driver has a delta to send.
///
/// @param movement The unit's movement record.
/// @return True when a resend is armed or the blocked bit differs from the one last sent.
[[nodiscard]] bool ground_driver_has_delta(const MovementRecord& movement) noexcept;

/// Captures a local ground driver's delta and records what was sent.
///
/// @param[in,out] movement The unit's movement record; its sent blocked bit is updated and the resend bit cleared.
/// @param[out] out Blocked bit and up to three route points (none unless the path is set).
void ground_driver_take_delta(MovementRecord* movement, WaypointDelta* out) noexcept;

/// Tells whether a local air driver has a delta to send.
///
/// @param movement The unit's movement record.
/// @return True when the air driver's resend bit is set.
[[nodiscard]] bool air_driver_has_delta(const MovementRecord& movement) noexcept;

/// Captures a local air driver's delta and records the substate sent.
///
/// @param[in,out] movement The unit's movement record; the sent substate bits are updated.
/// @param[out] out Goal tag and goal (omitted for goal kinds that are not replicated) and the substate.
void air_driver_take_delta(MovementRecord* movement, AirDelta* out) noexcept;

/// Re-arms a local air driver's delta when its substate moved since it was last sent.
///
/// @param[in,out] movement The unit's movement record.
void air_driver_note_substate(MovementRecord* movement) noexcept;

/// Packs the base fields of one unit into a full unit record.
///
/// @param world World holding the unit.
/// @param sim Hooks used to reach the unit's movement record, or null.
/// @param unit Unit to pack.
/// @return The record; only the def index (0) for an empty slot.
[[nodiscard]] FullUnitRecord
pack_full_unit_record(World* world, const ReplicationSim* sim, Unit* unit) noexcept;

/// Builds this tick's 0x2c record for a local player.
///
/// Lists every live mobile unit whose driver has a delta, stopping once the
/// record reaches 0x200 bytes, then appends the full record of the unit that
/// rides on this tick. Sets the player's last_sim_tick to Game.tick.
///
/// @param world World holding the player's units.
/// @param sim Hooks used to reach movement records, or null.
/// @param player_index Player slot, 0..9.
/// @param[in,out] writer Freshly initialised writer that receives the record.
/// @param[out] length Record length in bytes, when not null.
/// @return ok; bad_argument for an invalid player or null writer; the writer's error.
[[nodiscard]] WireError replication_pack_player(
    World* world,
    const ReplicationSim* sim,
    uint8_t player_index,
    BitWriter* writer,
    uint16_t* length
) noexcept;

// ---- receiver ----

/// Applies a received movement delta to a unit's remote driver.
///
/// A ground delta updates the blocked bit and route, then calls
/// apply_ground_delta. An air delta replaces the goal, calls set_air_goal,
/// then applies the substate.
///
/// @param world Receiving world.
/// @param sim Receiver hooks, or null.
/// @param unit Unit the delta is for.
/// @param[in,out] movement The unit's movement record; null does nothing.
/// @param delta Decoded delta.
void movement_apply_delta(
    World* world,
    const ReplicationSim* sim,
    Unit* unit,
    MovementRecord* movement,
    const UnitDelta& delta
) noexcept;

/// Applies a full unit record to its unit.
///
/// A zero def index marks a live unit for death. A changed def index first
/// recreates the unit through create_unit. Health, build fraction, state
/// flags and either the attachment or the position, angles and speed follow.
/// The record is read whole before any of it applies, and a recreated
/// unit's speed word, there when its def is mobile, before the unit is
/// recreated: a truncated record changes nothing.
///
/// @param world Receiving world.
/// @param sim Receiver hooks, or null.
/// @param[in,out] unit Unit the record is for.
/// @param[in,out] reader Reader positioned at the record; past it on success, unchanged otherwise.
/// @return ok; bad_argument when Game.unit_def_id_bits is outside 1..16; truncated.
[[nodiscard]] WireError replication_apply_full_record(
    World* world, const ReplicationSim* sim, Unit* unit, BitReader* reader
) noexcept;

/// Applies a 0x2c record from a sender.
///
/// Sets the sender's last_sim_tick, applies each listed delta, steps every
/// live mobile sender unit, then applies the full record when present.
/// Units outside the sender's range are rejected. A listed unit whose def
/// index changed is recreated only once its delta, in the layout its new
/// def's driver takes, has been read in full, so a body cut short recreates
/// nothing.
///
/// @param world Receiving world.
/// @param sim Receiver hooks, or null.
/// @param sender_index Player slot of the sender.
/// @param bytes Record start.
/// @param size Bytes readable from bytes.
/// @return ok; bad_argument for an invalid sender or def index width, or a unit outside the sender's range;
///         truncated; invalid_type; length_mismatch; unsupported_delta_layout for a unit with no delta layout.
[[nodiscard]] WireError replication_apply_unit_state(
    World* world,
    const ReplicationSim* sim,
    uint8_t sender_index,
    const uint8_t* bytes,
    std::size_t size
) noexcept;

/// Applies one in-game event record (0x09..0x14, 0x2c) from a sender as the packet pump does.
///
/// @param world Receiving world.
/// @param sim Receiver hooks, or null.
/// @param sender_index Player slot of the sender.
/// @param bytes Record start.
/// @param size Record size; must equal its wire length.
/// @param[out] handled Whether the type is one this function applies, when not null.
/// @return ok, or the decode error; bad_argument for a null world or bytes or size 0.
[[nodiscard]] WireError replication_apply_record(
    World* world,
    const ReplicationSim* sim,
    uint8_t sender_index,
    const uint8_t* bytes,
    std::size_t size,
    bool* handled
) noexcept;

} // namespace oa::netgame
