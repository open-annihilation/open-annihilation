// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "oa/sim/unit_movement/movement.hpp"

#include <span>

namespace oa::sim::unit_movement {

/// The result of a footprint query for local traffic steering.
enum class LocalFlowObstacle : uint8_t { clear, stationary, moving };

/// Describes the moving occupant selected by a bounded footprint query.
struct LocalFlowBlocker {
    LocalFlowObstacle obstacle{};
    uint16_t unit_id{};
    uint16_t heading{};
    uint32_t waiting_ticks{};
};

/// Counts local traffic work and its movement outcomes.
struct LocalFlowCounters {
    uint64_t probes{};               ///< footprint queries, including candidate clearance tests
    uint64_t moving_blockers{};      ///< route probes finding moving traffic
    uint64_t steering_adjustments{}; ///< temporary waypoints selected around traffic
    uint64_t yielded_ticks{};        ///< lower-priority traffic yielding or backing up
    uint64_t blocked_ticks{};        ///< final occupancy vetoes
    uint64_t deferred_replans{};     ///< mobile blockages held locally before requesting a path
};

/// Reads current occupancy without scanning all units or refreshing every projection.
struct LocalFlowProbe {
    void* context{};
    LocalFlowBlocker (*at)(
        void* context, const Unit& unit, std::array<int16_t, 2> cell, uint8_t occupancy
    ){};
};

/// A temporary steering target; the original mission and route remain unchanged.
struct LocalFlowDecision {
    std::array<Fixed, 3> target{};
    bool adjusted{};
    bool reverse{};
    bool mobile_blocked{};
};

/// A retained local steering head, shared with movement replication.
///
/// Its three points have the same origin, target and braking-point meaning as
/// a ground route. The original navigator still owns the mission and arrival.
struct LocalFlowHead {
    std::array<std::array<int16_t, 2>, 3> points{};
    uint64_t goal_key{}; ///< stable identity of the original goal's geometry
    uint32_t tick{};     ///< tick on which this effective head was used
    uint8_t count{};     ///< zero brakes; two or three points steer
    bool active{};
};

/// Canonical bytes per retained head, without native padding or pointers.
inline constexpr size_t local_flow_head_bytes = 26;

/// Writes retained heads as canonical little-endian save bytes.
///
/// @param heads retained heads
/// @param[out] bytes exactly heads.size() * local_flow_head_bytes bytes
/// @return false when the output size does not match
bool write_local_flow_heads(std::span<const LocalFlowHead> heads, std::span<uint8_t> bytes) noexcept;

/// Restores retained heads after validating the entire saved table.
///
/// @param[in,out] heads retained heads
/// @param bytes canonical little-endian save bytes
/// @return false without changing heads when sizes or head flags are invalid
bool read_local_flow_heads(std::span<LocalFlowHead> heads, std::span<const uint8_t> bytes) noexcept;

/// Chooses a short traffic-avoidance waypoint, with bounded footprint queries.
///
/// Only moving occupants cause yielding. Older waiting traffic takes priority,
/// with unit id breaking ties. Candidate segments retain the host's collision
/// veto, and diagonal segments test both adjoining cells. Turn and acceleration
/// limits remain the steering driver's responsibility.
///
/// @param unit moving unit
/// @param movement current motion; last_motion_tick is the last actual progress tick
/// @param target original next steering point
/// @param tick current simulation tick
/// @param host final occupancy queries
/// @param probe current traffic occupants
/// @param[in,out] counters query and yielding counts
/// @return original target, a clear local target, or a backwards yielding step
[[nodiscard]] LocalFlowDecision choose_local_flow(
    const Unit& unit,
    const Movement& movement,
    std::array<Fixed, 3> target,
    uint32_t tick,
    Host& host,
    LocalFlowProbe probe,
    LocalFlowCounters& counters
);

/// Integrates v2 ground movement while retaining the final occupancy veto.
///
/// The saved last-motion tick advances only on actual position changes. A
/// transient moving blockage can wait locally before setting collision_blocked
/// and asking the navigator to find another route. No unit is moved through an
/// occupied footprint.
///
/// @param[in,out] unit unit to move
/// @param[in,out] movement motion and last actual progress tick
/// @param tick current simulation tick
/// @param local_simulation whether this machine owns the movement
/// @param mobile_blocked whether the route probe found moving traffic
/// @param host collision and spatial operations
/// @param[in,out] counters blockage and deferred-replan counts
void integrate_local_flow(
    Unit& unit,
    Movement& movement,
    uint32_t tick,
    bool local_simulation,
    bool mobile_blocked,
    Host& host,
    LocalFlowCounters& counters
);

} // namespace oa::sim::unit_movement
