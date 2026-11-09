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
    /// Transient proof of a compatible leader within the caller's traffic grace period.
    bool same_route_queue{};
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

struct LocalFlowHead;

/// Reads current occupancy without scanning all units or refreshing every projection.
struct LocalFlowProbe {
    void* context{};
    LocalFlowBlocker (*at)(
        void* context, const Unit& unit, std::array<int16_t, 2> cell, uint8_t occupancy
    ){};
    /// Last progress ticks captured before any unit moves this tick, indexed by unit id.
    /// An empty span uses the caller's current movement clock.
    std::span<const uint32_t> progress_at_tick_start{};
    /// Leg completed on this or the preceding tick for the same goal; null supplies no history.
    const LocalFlowHead* previous_head{};
    /// Whether a mature retry may undo a completed leg to explore a passing place.
    bool allow_reverse{true};
    /// Whether a mature, stationary retry may voluntarily yield its traffic priority.
    /// The caller schedules this from the existing retry phase and actual motion age.
    bool recovery_yield{};
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
/// An inactive head with two points retains a just-completed leg for one tick.
struct LocalFlowHead {
    std::array<std::array<int16_t, 2>, 3> points{};
    uint64_t goal_key{}; ///< stable identity of the original goal's geometry
    uint32_t tick{};     ///< tick used while active, or the completion tick while inactive
    uint8_t count{};     ///< zero brakes; two or three points steer
    bool active{};
    bool mobile_blocked{};   ///< previous route probe found moving traffic
    uint8_t blocked_ticks{}; ///< consecutive occupancy vetoes, saturated at 31
};

/// Canonical bytes per retained head, without native padding or pointers.
inline constexpr size_t local_flow_head_bytes = 27;

/// Number of consecutive blocked ticks before the navigator retries the route.
inline constexpr uint8_t local_flow_retry_ticks = 30;

/// Point count marking a completed, inactive local leg for one following tick.
inline constexpr uint8_t local_flow_completed_count = 2;

/// Tests whether a temporary steering waypoint has been reached or passed.
///
/// A unit within five world units has arrived. Crossing the target's forward
/// plane also completes the leg, so a unit whose turn radius exceeds this
/// short detour cannot keep circling an already-passed waypoint. Empty braking
/// heads do not complete through this test.
///
/// @param unit moving unit
/// @param head retained local steering head
/// @return whether the original route should resume
[[nodiscard]] bool local_flow_head_reached(const Unit& unit, const LocalFlowHead& head) noexcept;

/// Tests whether an inactive local leg completed on the preceding tick.
///
/// The caller must also check the original goal's identity. Replaced or
/// expired active heads supply no history.
///
/// @param head retained local traffic state
/// @param tick current simulation tick
/// @return whether the completed leg remains recent enough for local avoidance
[[nodiscard]] bool
local_flow_completed_last_tick(const LocalFlowHead& head, uint32_t tick) noexcept;

/// Records an occupancy result independently of subpixel motion within a cell.
///
/// @param[in,out] head retained local traffic state
/// @param blocked whether the final occupancy veto remains set
void record_local_flow_block(LocalFlowHead& head, bool blocked) noexcept;

/// Tests whether a transient traffic blockage can wait before replanning.
///
/// @param head retained local traffic state
/// @param movement authoritative movement and collision result
/// @return true only during the bounded local traffic retry window
[[nodiscard]] bool
local_flow_defer_replan(const LocalFlowHead& head, const Movement& movement) noexcept;

/// Schedules a bounded local retry after sustained traffic blockage.
///
/// Ordinary path replanning remains enabled. Unit IDs stagger local recovery
/// over the thirty-tick retry period, allowing a newly opened passing place
/// to be used without restarting the transient-blockage grace period.
///
/// @param head retained local traffic state
/// @param tick current simulation tick
/// @param unit_id stable unit slot
/// @return whether an old local leg should expire and a new one may be chosen
[[nodiscard]] bool
local_flow_recovery_due(const LocalFlowHead& head, uint32_t tick, uint16_t unit_id) noexcept;

/// Writes retained heads as canonical little-endian save bytes.
///
/// @param heads retained heads
/// @param[out] bytes exactly heads.size() * local_flow_head_bytes bytes
/// @return false when the output size does not match
bool write_local_flow_heads(
    std::span<const LocalFlowHead> heads, std::span<uint8_t> bytes
) noexcept;

/// Restores retained heads after validating the entire saved table.
///
/// @param[in,out] heads retained heads
/// @param bytes canonical little-endian save bytes
/// @return false without changing heads when sizes or head flags are invalid
bool read_local_flow_heads(std::span<LocalFlowHead> heads, std::span<const uint8_t> bytes) noexcept;

/// Chooses a short traffic-avoidance waypoint, with bounded footprint queries.
///
/// Only mobile ground occupants, including idle units, cause yielding.
/// Older waiting traffic takes priority,
/// with unit id breaking ties. Both ages use the same tick-start snapshot so
/// an earlier unit update cannot change another unit's priority. Candidate segments retain the host's collision
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

/// Inspects the controller's aim while preserving the original passing direction.
///
/// Only the traffic scan follows probe_target. Candidate angles and completed
/// leg history continue to use the original target. Supplying the same point
/// for both targets is equivalent to the single-target overload.
///
/// @param unit moving unit
/// @param movement current motion and actual progress clock
/// @param target original next route point used for passing direction
/// @param probe_target actual controller aim used to inspect traffic
/// @param tick current simulation tick
/// @param host final occupancy queries
/// @param probe current traffic occupants
/// @param[in,out] counters query and yielding counts
/// @return original target or a clear local passing target
[[nodiscard]] LocalFlowDecision choose_local_flow(
    const Unit& unit,
    const Movement& movement,
    std::array<Fixed, 3> target,
    std::array<Fixed, 3> probe_target,
    uint32_t tick,
    Host& host,
    LocalFlowProbe probe,
    LocalFlowCounters& counters
);

/// Integrates v2 ground movement while retaining the final occupancy veto.
///
/// The saved last-motion tick advances only on actual position changes. A
/// collision flag remains authoritative for mirrored movement. A caller can
/// defer navigation retries for transient mobile traffic before steering.
/// No unit is moved through an occupied footprint.
///
/// @param[in,out] unit unit to move
/// @param[in,out] movement motion and last actual progress tick
/// @param tick current simulation tick
/// @param local_simulation whether this machine owns the movement
/// @param host collision and spatial operations
/// @param[in,out] counters blockage and deferred-replan counts
void integrate_local_flow(
    Unit& unit,
    Movement& movement,
    uint32_t tick,
    bool local_simulation,
    Host& host,
    LocalFlowCounters& counters
);

/// Opts into the swept ground step while retaining Enhanced motion-clock accounting.
///
/// The actual-position progress clock and blocked counter follow integrate_local_flow:
/// a step leaving position unchanged restores its previous clock. Other layers and
/// remote simulation retain native integration through the swept function's delegation.
/// The caller must establish that the unit is unattached.
///
/// @param[in,out] unit actual unit whose pose and occupancy are updated
/// @param[in,out] movement actual motion, speed, collision flags and progress clock
/// @param tick current simulation tick
/// @param local_simulation whether this machine simulates the unit's owner
/// @param host existing native collision and spatial operations
/// @param[in,out] counters existing Enhanced blocked-step accounting
/// @return the swept step's bounded-query outcome
[[nodiscard]] GroundSweepResult integrate_swept_local_flow(
    Unit& unit,
    Movement& movement,
    uint32_t tick,
    bool local_simulation,
    Host& host,
    LocalFlowCounters& counters
);

} // namespace oa::sim::unit_movement
