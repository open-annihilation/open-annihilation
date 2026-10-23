// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Read-only hooks that report what happens in a match: units created and
// finished, shots placed, shots detonating, damage and deaths. The match
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

#include <cstddef>
#include <cstdint>

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
};

} // namespace oa::sim::match_runtime
