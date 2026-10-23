// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Events a seated brain hears at its turn. The listener records them while
// the tick runs; the seat delivers them at its controller point.
#pragma once

#include <cstdint>

namespace oa::sim::match_runtime {
class Match;
}

namespace oa::sim::brains {

/// What one queued event is.
///
/// `wreck` is not a hook. Delivery attaches it to the death it follows.
enum class EventKind : uint8_t {
    created,     ///< a unit was created
    finished,    ///< a unit finished building
    damaged,     ///< a unit took damage, paralysis or healing
    died,        ///< a unit died
    wreck,       ///< a corpse was placed for a death
    given,       ///< a unit was given to another player
    idle,        ///< a unit ran out of orders
    order_ended, ///< an order ended
    carried,     ///< a unit joined a carrier
    dropped,     ///< a unit left a carrier
    seen,        ///< an enemy entered the seen list
    lost,        ///< an enemy left the seen list
    path_failed, ///< a unit cannot reach its order's goal
};

/// One event recorded when it fires.
///
/// The unit and the other unit keep the generation they had then. A slot
/// reused before the seat's turn is not this unit.
struct Event {
    /// Which event this is.
    EventKind kind{};
    /// Damage kind, death kind or order kind. Unused for the other events.
    uint8_t detail{};
    /// Gift's old owner, the dying unit's owner, or the attacker's owner.
    uint8_t from{};
    /// Gift's new owner, the killer's owner, or 1 when an order finished.
    uint8_t to{};
    /// The unit the event is about.
    uint16_t unit_slot{};
    /// Builder, attacker, killer, copy, carrier or nothing (slot 0).
    uint16_t other_slot{};
    /// Game.tick when the event fired.
    uint32_t tick{};
    /// Generation of `unit_slot` when the event fired.
    uint32_t unit_generation{};
    /// Generation of `other_slot` when the event fired.
    uint32_t other_generation{};
    /// Damage or healing, as a whole number of health points.
    int32_t amount{};
    /// FeatureDef index of a wreck. Meaningful when `kind` is `wreck`.
    uint16_t feature{};
    /// Wreck position, east, 16.16 map units.
    int32_t x{};
    /// Wreck position, south, 16.16 map units.
    int32_t z{};
};

/// Most events one seat keeps for a single turn. A further event is dropped.
inline constexpr uint32_t max_events_per_tick = 65536;

/// Returns how many events this seat has dropped.
///
/// The count starts at 0 and grows when a turn would pass `max_events_per_tick`
/// or a buffer cannot be grown. A player with no brain has dropped none.
///
/// @param match the match
/// @param player player index, 0 to 9
/// @return the dropped count, or 0 when that seat has no brain
[[nodiscard]] uint32_t dropped_events(const match_runtime::Match& match, uint8_t player) noexcept;

} // namespace oa::sim::brains
