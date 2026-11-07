// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Records a replayed game's timeline (timeline.hpp) as the replay runs: the
// match's event hooks (oa/sim/match_runtime/event_hooks.hpp) report its events,
// and after each tick the recorder samples the world. It reads the world
// and nothing else: it never writes match state or draws from the match's
// random streams, so a replay recorded plays out as one that is not.
#pragma once

#include "oa/core/world.h"
#include "oa/media/director/timeline.hpp"
#include "oa/sim/match_runtime/event_hooks.hpp"

#include <cstdint>
#include <exception>
#include <string_view>
#include <vector>

namespace oa::media::director {

/// Tells whether the viewer's view draws a unit: not a cloaked enemy, not a
/// submerged unit without a sonar contact.
struct UnitVisibilityHooks {
    void* context{};
    /// Tells whether the viewer's view draws a unit.
    ///
    /// @param context UnitVisibilityHooks::context
    /// @param unit the unit's slot
    /// @return true when the unit is drawn; null counts every unit as drawn
    bool (*visible)(void* context, uint16_t unit){};
};

/// What a recorder keeps of a replay.
enum class Detail : uint8_t {
    /// Everything the generator plans from: every unit created, finished and
    /// killed, every shot, detonation and hit, the mobile units twice a
    /// second, the types and weapons used and the map's water.
    full,
    /// What a replay's index reads: the players' standings, every hit, and
    /// the commanders created and killed. The recorder asks the match for
    /// those events only, and keeps no sample of the units, so a long
    /// replay's timeline stays small.
    standings,
};

/// Reads how every player of the match stands.
///
/// @param world the match's world
/// @param tick the tick to name in each entry
/// @return one entry for each player slot in use, by player index; the same
///         entries after_tick records on a stats tick
[[nodiscard]] std::vector<PlayerStats> read_player_stats(const oa::World& world, uint32_t tick);

/// Records one replay's timeline.
class TimelineRecorder {
  public:

    TimelineRecorder() = default;
    TimelineRecorder(const TimelineRecorder&) = delete;
    TimelineRecorder& operator=(const TimelineRecorder&) = delete;

    /// Starts the timeline from the world as the replay starts: the map and
    /// its water, the players and the tick.
    ///
    /// @param world the match's world, before its first replayed tick
    /// @param viewer_player the player index the replay is watched from
    /// @param map_name the map's name, as the game shows it
    /// @param detail what to keep; `standings` leaves out the water, the
    ///        units other than commanders already in the world, and whatever
    ///        event_hooks and after_tick would record beyond the standings
    void begin(
        const oa::World& world,
        uint8_t viewer_player,
        std::string_view map_name,
        Detail detail = Detail::full
    );

    /// Returns the event hooks that record the match's events.
    ///
    /// @return hooks whose context is this recorder, with a hook only for
    ///         each event the recorder keeps at its detail; install them as
    ///         Match::event_hooks for the replay and remove them before the
    ///         recorder goes
    [[nodiscard]] oa::sim::match_runtime::EventHooks event_hooks() noexcept;

    /// Samples the world after a tick: the live mobile units on every
    /// sample_period_ticks-th tick (at full detail only), the players on
    /// every stats_period_ticks-th, counted from the first tick.
    ///
    /// @param world the match's world, after the tick
    /// @param visibility which units the viewer's view draws
    void after_tick(const oa::World& world, const UnitVisibilityHooks& visibility);

    /// Ends the timeline: its last tick, its verdict and its usable end.
    ///
    /// @param world the match's world, after the last tick replayed
    /// @param verdict how the replay went
    void finish(const oa::World& world, const ReplayVerdict& verdict);

    /// Hands the recorded timeline over; the recorder is empty afterwards.
    ///
    /// @return the timeline
    [[nodiscard]] Timeline take();

  private:

    /// Records a created unit (EventHooks::unit_created).
    ///
    /// @param context the recorder
    /// @param world the match's world
    /// @param unit the unit's slot
    static void unit_created(void* context, const oa::World& world, uint16_t unit);

    /// Records a finished unit (EventHooks::unit_finished).
    ///
    /// @param context the recorder
    /// @param world the match's world
    /// @param unit the unit's slot
    /// @param builder the builder's slot
    static void
    unit_finished(void* context, const oa::World& world, uint16_t unit, uint16_t builder);

    /// Records a placed shot (EventHooks::shot_placed).
    ///
    /// @param context the recorder
    /// @param world the match's world
    /// @param shot the shot's record
    /// @param source how the shot came to be
    /// @param aim the aim point, or null
    /// @param target_unit the unit aimed at, or 0
    static void shot_placed(
        void* context,
        const oa::World& world,
        const oa::Projectile& shot,
        oa::sim::match_runtime::ShotSource source,
        const oa::FixedVec3* aim,
        uint16_t target_unit
    );

    /// Records a detonation (EventHooks::shot_detonated).
    ///
    /// @param context the recorder
    /// @param world the match's world
    /// @param shot the shot's record
    /// @param direct_unit the unit struck, or 0
    static void shot_detonated(
        void* context, const oa::World& world, const oa::Projectile& shot, uint16_t direct_unit
    );

    /// Records a health event (EventHooks::unit_damaged).
    ///
    /// @param context the recorder
    /// @param world the match's world
    /// @param target the unit's slot
    /// @param source the attacker's slot, or 0
    /// @param amount health points
    /// @param kind the health event's kind
    static void unit_damaged(
        void* context,
        const oa::World& world,
        uint16_t target,
        uint16_t source,
        int16_t amount,
        uint8_t kind
    );

    /// Records a death (EventHooks::unit_died).
    ///
    /// @param context the recorder
    /// @param world the match's world
    /// @param unit the dying unit's slot
    /// @param outcome how the death comes out
    /// @param settled_elsewhere true for a death the recording settled
    static void unit_died(
        void* context,
        const oa::World& world,
        uint16_t unit,
        const oa::sim::match_runtime::KillOutcome& outcome,
        bool settled_elsewhere
    );

    Timeline timeline_{};
    Detail detail_{Detail::full}; ///< what begin was asked to keep
    uint32_t sampled_tick_{};     ///< the last tick after_tick sampled; valid when sampled_
    bool sampled_{};              ///< after_tick has sampled a tick
    /// What stopped an event hook from recording (an allocation that
    /// failed); the hooks never let it into the match, and finish and take
    /// throw it.
    std::exception_ptr failure_{};
};

} // namespace oa::media::director
