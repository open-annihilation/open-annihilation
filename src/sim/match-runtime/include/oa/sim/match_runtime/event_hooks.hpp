// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Read-only hooks that report what happens in a match: units created and
// which unit started them, units finished, units given to another player,
// a unit going idle, an order ending, shots placed, shots detonating,
// damage and deaths, a unit carried or dropped, the wreck a death
// leaves, the enemies a player has gained or lost sight of, and a goal
// a unit cannot reach. The match
// reports each event to its primary table (Match::event_hooks), then to
// each listener in the order it was added, at the points every such event
// passes through, whoever simulates the unit: this machine, or a recording
// the match replays. A hook reads the world it is given and nothing else: it
// never writes match state, draws from the match's random streams or calls
// back into the match, and it never adds or removes a listener. The same
// rule binds every listener, so a match with event hooks plays out exactly
// as one without.
#pragma once

#include "oa/core/projectile.h"
#include "oa/core/types.h"
#include "oa/core/world.h"
#include "oa/sim/detection.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace oa::sim::match_runtime {

/// Most listeners one match holds. One more add returns no id.
inline constexpr std::size_t max_event_listeners = 8;

/// Identity of one listener. 0 is no listener, and an id is never handed out
/// twice in one match.
using EventListenerId = uint32_t;

struct KillOutcome;

/// How a placed shot came to be.
enum class ShotSource : uint8_t {
    weapon, ///< a unit's weapon fired it, on this machine or as recorded
    burst,  ///< a burst shot copied from the shot before it
    meteor, ///< the meteor storm launched it
};

/// The match's event hooks; each null member reports nothing.
struct EventHooks {
    void* context{};

    /// Reports a unit created in a slot, once its record is filled; null
    /// reports none.
    ///
    /// @param context EventHooks::context
    /// @param world the match's world
    /// @param unit the unit's slot
    void (*unit_created)(void* context, const oa::World& world, uint16_t unit){};

    /// Reports a unit finished: its build progress reached the end; null
    /// reports none.
    ///
    /// @param context EventHooks::context
    /// @param world the match's world
    /// @param unit the finished unit's slot
    /// @param builder the slot of the unit whose work finished it; `unit` for
    ///        a building created finished
    void (*unit_finished)(void* context, const oa::World& world, uint16_t unit, uint16_t builder){};

    /// Reports a shot placed in the projectile pool, before its first flight
    /// step; null reports none.
    ///
    /// @param context EventHooks::context
    /// @param world the match's world
    /// @param shot the shot's record; valid for the call only (the pool is
    ///        compacted every tick)
    /// @param source how the shot came to be
    /// @param aim the point the shot was aimed at, 16.16 map units; null for a
    ///        shot with none. For a recorded shot this is the recorded aim,
    ///        which the record's own target field does not keep
    /// @param target_unit the unit it was aimed at; 0 for none
    void (*shot_placed)(
        void* context,
        const oa::World& world,
        const oa::Projectile& shot,
        ShotSource source,
        const oa::FixedVec3* aim,
        uint16_t target_unit
    ){};

    /// Reports a shot detonating, before its blast, damage and sound; null
    /// reports none.
    ///
    /// Called once for every detonation, including each repeated one of a
    /// shot that does not explode on contact.
    ///
    /// @param context EventHooks::context
    /// @param world the match's world
    /// @param shot the shot's record, at the point it detonates
    /// @param direct_unit the unit it struck; 0 for none
    void (*shot_detonated)(
        void* context, const oa::World& world, const oa::Projectile& shot, uint16_t direct_unit
    ){};

    /// Reports a health event applied to a unit: weapon damage, paralysis,
    /// healing or a recorded health change; null reports none.
    ///
    /// @param context EventHooks::context
    /// @param world the match's world, before the event changes the unit
    /// @param target the unit's slot
    /// @param source the attacker's slot; 0 for none
    /// @param amount health points taken (healing given for kind 10)
    /// @param kind Match::apply_damage_event's kind: 1 weapon, 2 paralyze,
    ///        10 healing, 11 not shared, or a DeathKind
    void (*unit_damaged)(
        void* context,
        const oa::World& world,
        uint16_t target,
        uint16_t source,
        int16_t amount,
        uint8_t kind
    ){};

    /// Reports a unit's death as its teardown starts, while its record still
    /// holds its type, owner, position and last attacker; null reports none.
    ///
    /// @param context EventHooks::context
    /// @param world the match's world
    /// @param unit the dying unit's slot
    /// @param outcome how the death comes out
    /// @param settled_elsewhere true for a death the unit's own machine or
    ///        the recording settled
    void (*unit_died)(
        void* context,
        const oa::World& world,
        uint16_t unit,
        const KillOutcome& outcome,
        bool settled_elsewhere
    ){};

    /// Reports which unit's work started a new unit, right after unit_created
    /// for the same unit and before unit_finished when a building is created
    /// finished; null reports none.
    ///
    /// @param context EventHooks::context
    /// @param world the match's world
    /// @param unit the new unit's slot
    /// @param builder the slot of the unit whose work started it: the mobile
    ///        builder of a nanoframe, the factory of a factory's frame, the
    ///        resurrecting unit of a raised one, or the aircraft of a building
    ///        built from the air. 0 when no unit made it: a commander, a
    ///        mission's unit, a hand-over's copy, or a copy of another
    ///        machine's unit
    void (*unit_created_by)(
        void* context, const oa::World& world, uint16_t unit, uint16_t builder
    ){};

    /// Reports one capture, share-panel gift or structure gift, after the copy
    /// is created and reported created when this machine makes it, and before
    /// the old unit's death; null reports none. A transfer that changes
    /// nothing is not reported.
    ///
    /// @param context EventHooks::context
    /// @param world the match's world
    /// @param unit the old unit's slot, still live
    /// @param copy the new unit's slot, or 0 when the receiver's own machine
    ///        makes the copy
    /// @param from the old owner's player index
    /// @param to the new owner's player index
    void (*unit_given)(
        void* context,
        const oa::World& world,
        uint16_t unit,
        uint16_t copy,
        uint8_t from,
        uint8_t to
    ){};

    /// Reports a finished unit simulated here that ran out of orders and was
    /// given its default mission; null reports none.
    ///
    /// Units without a movement object, and default missions the match does
    /// not run, are not reported. A frame waiting for its builder is not idle.
    ///
    /// @param context EventHooks::context
    /// @param world the match's world
    /// @param unit the idle unit's slot
    void (*unit_idle)(void* context, const oa::World& world, uint16_t unit){};

    /// Reports an order of a unit simulated here that its handler ended, just
    /// before it is removed; null reports none.
    ///
    /// The kind is `Order::kind`. `done` is true when the handler finished the
    /// order and false when it failed. Orders removed with it by a clear,
    /// orders replaced by a new command, the secondary queue's orders and
    /// orders of a dying unit are not reported.
    ///
    /// @param context EventHooks::context
    /// @param world the match's world
    /// @param unit the unit's slot
    /// @param kind the order's mission kind
    /// @param done true when the handler finished the order, false when it failed
    void (*order_ended)(
        void* context, const oa::World& world, uint16_t unit, uint8_t kind, bool done
    ){};

    /// Reports a carry link that changed, on this machine or from another
    /// machine's record, after the link is in place; null reports none.
    ///
    /// The unit is reported once with `loaded` false and the carrier it left,
    /// then once with `loaded` true and the carrier it joined. A link that
    /// only changes the piece, and a link that was refused, are not reported.
    ///
    /// @param context EventHooks::context
    /// @param world the match's world
    /// @param unit the carried unit's slot
    /// @param carrier the carrier it left or joined
    /// @param loaded false when it left that carrier, true when it joined it
    void (*unit_carried)(
        void* context, const oa::World& world, uint16_t unit, uint16_t carrier, bool loaded
    ){};

    /// Reports a corpse a death placed, after that unit's `unit_died`, while
    /// the dead unit's record still holds its position; null reports none.
    ///
    /// @param context EventHooks::context
    /// @param world the match's world
    /// @param unit the dead unit's slot
    /// @param feature the placed feature's type, a FeatureDef index
    /// @param plot the index into World::plots the corpse was placed in
    void (*wreck_placed)(
        void* context, const oa::World& world, uint16_t unit, uint16_t feature, uint32_t plot
    ){};

    /// Reports the enemies a player gained and lost sight of, at the end of
    /// that player's knowledge refresh when its seen list changed; null
    /// reports none.
    ///
    /// The spans are valid for the call only. A lost unit may have died, or
    /// its slot may hold another unit now, which a reader tells from the
    /// world and the generation.
    ///
    /// @param context EventHooks::context
    /// @param world the match's world
    /// @param player the player whose seen list changed
    /// @param gained enemies that entered the seen list, in slot order
    /// @param lost enemies that left the seen list, in slot order
    void (*seen_changed)(
        void* context,
        const oa::World& world,
        uint8_t player,
        std::span<const oa::sim::detection::SightedUnit> gained,
        std::span<const oa::sim::detection::SightedUnit> lost
    ){};

    /// Reports that a unit simulated here cannot reach its order's goal.
    ///
    /// A route search for that goal found no route, and the unit was not
    /// already at the goal. An order is reported once, during the tick's
    /// path search, even when it keeps trying. Null reports none.
    ///
    /// @param context EventHooks::context
    /// @param world the match's world
    /// @param unit the unit's slot
    /// @param kind the order's mission kind
    void (*path_failed)(void* context, const oa::World& world, uint16_t unit, uint8_t kind){};
};

} // namespace oa::sim::match_runtime
