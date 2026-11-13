// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "tick_internal.hpp"
#include "enhanced_state.hpp"
#include "live_route_corner.hpp"
#include "local_throat_guide.hpp"
#include "route_queue.hpp"

#include "oa/data/enhanced_mode.hpp"
#include "oa/sim/air/flight.hpp"
#include "oa/sim/air/host.hpp"
#include "oa/sim/ground_orders/steering_target.hpp"
#include "oa/sim/world_environment/wind.hpp"

#include <cstdint>

namespace oa::sim::match_runtime {
namespace {
constexpr uint8_t ground_occupancy = 1;
constexpr int32_t live_route_corner_version = 3;
constexpr uint32_t local_throat_probe_period = 12;

/// Identifies goal geometry without retaining order pointers or native padding.
///
/// @param goal original navigation goal
/// @return stable geometry key
uint64_t local_flow_goal_key(const sim::ground_orders::Goal& goal) noexcept {
    uint64_t key = 14695981039346656037ull;
    const auto add = [&key](uint32_t value) {
        for (unsigned byte = 0; byte < 4; ++byte) {
            key ^= static_cast<uint8_t>(value >> (byte * 8));
            key *= 1099511628211ull;
        }
    };
    add(static_cast<uint32_t>(goal.shape));
    add(static_cast<uint16_t>(goal.cell[0]));
    add(static_cast<uint16_t>(goal.cell[1]));
    add(static_cast<uint32_t>(goal.tolerance));
    add(static_cast<uint32_t>(goal.radius_squared));
    add(static_cast<uint32_t>(goal.inner_range));
    add(static_cast<uint32_t>(goal.inner_radius_squared));
    add(static_cast<uint32_t>(goal.left));
    add(static_cast<uint32_t>(goal.right));
    add(static_cast<uint32_t>(goal.top));
    add(static_cast<uint32_t>(goal.bottom));
    return key;
}

/// Converts a unit centre to the whole-world coordinates used by route heads.
///
/// @param unit moving unit
/// @return current X/Z route point
sim::ground_orders::RoutePoint local_flow_point(const sim::unit_movement::Unit& unit) noexcept {
    return {
        static_cast<int16_t>(unit.position[0] >> 16), static_cast<int16_t>(unit.position[2] >> 16)
    };
}

} // namespace

sim::air::AirHost TickHost::air_host() {
    sim::air::AirHost host{};
    host.context = &match;
    host.world = &match.state();
    host.bucket_height = [](void* context, const oa::Unit* unit) -> uint8_t {
        const auto& m = *static_cast<Match*>(context);
        if (unit->id >= m.spatial_units_.size())
            return m.spatial_.outside_bucket.area_high_height;
        return sim::spatial_state::unit_bucket(m.spatial_, m.spatial_units_[unit->id])
            .area_high_height;
    };
    // The off-map bucket as of the unit's last cell change, from its whole
    // footprint rather than its current position.
    host.outside_map = [](void* context, const oa::Unit* unit) {
        const auto& m = *static_cast<Match*>(context);
        if (unit->id >= m.spatial_units_.size())
            return false;
        const auto& projected = m.spatial_units_[unit->id];
        return projected.bucket_linked && !projected.bucket;
    };
    // A goal over one of a unit's pieces (a landing pad's) follows that
    // piece; -1 is the unit's own position.
    host.query_point = [](void* context, const oa::Unit* unit, int16_t index) {
        auto& m = *static_cast<Match*>(context);
        if (index < 0 || unit->id >= m.slots_.size() || !m.instance(unit->id))
            return unit->position;
        const auto at = m.piece_world_position(m.slots_[unit->id], static_cast<uint32_t>(index));
        return oa::FixedVec3{
            std::bit_cast<int32_t>(at[0]),
            std::bit_cast<int32_t>(at[1]),
            std::bit_cast<int32_t>(at[2])
        };
    };
    host.terrain_height = [](void* context, oa::oa_fixed x, oa::oa_fixed z) {
        return static_cast<Match*>(context)->sample_terrain_height(
            static_cast<uint32_t>(x), static_cast<uint32_t>(z)
        );
    };
    return host;
}

void TickHost::movement_tick(oa::Unit& record) {
    auto& u = unit_view(record);
    auto& s = slot(u);
    if (!match.ground_runtime(s.unit_index))
        return;
    auto& g = ground(u);
    g.project_slot();
    auto flags = weapon_flags(s);
    const auto parent_index = link_parent(s.record);
    auto& driver = match.air_drivers_.at(s.unit_index);
    const auto host = air_host();
    // A flying unit's movement object has the air driver, and the air driver
    // alone moves it: its missions hand their goals to the driver, never to
    // the ground navigator.
    const bool air_driven = driver.unit != nullptr;
    const bool enhanced_flow = match.enhanced_ && match.rules().setup.enhanced_mode.enabled &&
                               match.rules().setup.enhanced_mode.version >= 2;
    auto* flow_head = enhanced_flow && s.unit_index < match.enhanced_->unit_count
                          ? &match.enhanced_->local_flow_heads[s.unit_index]
                          : nullptr;
    if (flow_head && (air_driven || parent_index || g.mirrored_driver)) {
        if (flow_head->active)
            g.navigation.flags |= sim::ground_orders::route_changed_flag;
        *flow_head = {};
        if (match.enhanced_->advisory &&
            (match.enhanced_->advisory->guides[s.unit_index].phase != AdvisoryLanePhase::none ||
             match.enhanced_->advisory->published_guides[s.unit_index].active))
            match.reset_advisory_unit(s.unit_index);
    }
    // The driver's per-tick slot and the steering step run for carried units
    // too; the carried branch below then overrides their position, attitude,
    // velocity and speed but keeps the turn rate and filtered vector.
    if (air_driven) {
        sim::air::air_driver_update(&driver, host, g.movement.flags & 3);
        if (const oa::UnitDef* def = oa::world_unit_def_of(&match.state(), &s.record)) {
            int16_t bank = 0;
            sim::air::air_flight_step(
                g.geometry,
                g.movement,
                g.previous_vector,
                sim::air::air_driver_steering_target(&driver),
                *def,
                match.state().game.gravity != 0 ? match.state().game.gravity
                                                : sim::ground_orders::default_gravity,
                host.outside_map(host.context, &s.record),
                &bank
            );
            if ((g.movement.flags & 3) == sim::air::layer_air) {
                s.record.bank = bank;
                s.record.pitch = g.geometry.pitch;
            }
        }
    } else if (g.mirrored_driver) {
        // The mirrored navigator's per-tick slot does nothing; the
        // shared route head alone steers.
        sim::ground_orders::steer_ground(
            g.geometry,
            g.movement,
            g.mirrored_navigation,
            g.acceleration,
            g.deceleration,
            match.simulation_.sea_level,
            fast_unit_steering()
        );
    } else {
        // Keep the authoritative collision result for replication; only the
        // navigator's retry decision ignores a short-lived mobile blockage.
        const auto collision = g.movement.flags & sim::unit_movement::collision_blocked;
        const bool defer_replan =
            flow_head && sim::unit_movement::local_flow_defer_replan(*flow_head, g.movement);
        if (defer_replan) {
            g.movement.flags &= uint8_t(~sim::unit_movement::collision_blocked);
            ++match.enhanced_->local_flow_counters.deferred_replans;
        }
        sim::ground_orders::tick_navigation(view(s, g, flags), match.simulation_.tick, *this);
        if (defer_replan)
            g.movement.flags |= collision;
        bool advanced_corner = false;
        const bool moving_corner =
            flow_head &&
            data::enhanced_mode::uses_distributed_battle_flow(
                match.rules().setup.enhanced_mode.version
            ) &&
            g.movement.speed > 0 &&
            live_route_corner_moving_due(
                g.geometry, g.navigation, match.simulation_.tick, s.unit_index
            );
        const bool blocked_corner =
            flow_head &&
            live_route_corner_due(flow_head->blocked_ticks, match.simulation_.tick, s.unit_index);
        const bool head_joins_corner =
            !flow_head || !flow_head->active ||
            (flow_head->count == 3 && flow_head->points[2] == g.navigation.points[2]);
        if (flow_head && match.rules().setup.enhanced_mode.version >= live_route_corner_version &&
            !parent_index && head_joins_corner &&
            (g.movement.flags & sim::unit_movement::occupancy_mask) == ground_occupancy &&
            (blocked_corner || moving_corner)) {
            if (const auto* klass = match.movement_class_map(g.movement_class)) {
                auto classifier = klass->record;
                classifier.occupancy_before_tick = 0;
                const auto player = s.record.owner_index;
                const auto known_static_clear = [&](int16_t x, int16_t z) {
                    if (player >= OA_PLAYER_COUNT || x < 0 || z < 0 ||
                        uint32_t(x) + uint32_t(classifier.footprint_x) >=
                            match.spatial_.terrain_width ||
                        uint32_t(z) + uint32_t(classifier.footprint_z) >=
                            match.spatial_.terrain_height)
                        return false;
                    const auto sight_width = match.spatial_.terrain_width >> 1;
                    const auto sight_height = match.spatial_.terrain_height >> 1;
                    for (int32_t dz = 0; dz < classifier.footprint_z; ++dz)
                        for (int32_t dx = 0; dx < classifier.footprint_x; ++dx) {
                            const auto sight_x = uint32_t(x + dx) >> 1;
                            const auto sight_z = uint32_t(z + dz) >> 1;
                            const auto index = size_t(sight_z) * sight_width + sight_x;
                            if (sight_x >= sight_width || sight_z >= sight_height ||
                                index >= match.sight_.player_bits.size() ||
                                (match.sight_.player_bits[index] & (1u << player)) == 0)
                                return false;
                        }
                    return sim::ground_orders::static_flow_passable(
                        classifier, x, z, match.spatial_
                    );
                };
                advanced_corner = try_advance_visible_corner(
                    g.geometry,
                    g.navigation,
                    classifier.footprint_x,
                    classifier.footprint_z,
                    known_static_clear,
                    match.enhanced_->local_flow_counters.probes,
                    blocked_corner ? live_route_corner_maximum_plots
                                   : moving_route_corner_maximum_plots
                );
            }
        }
        auto effective_navigation = g.navigation;
        if (match.enhanced_ && match.enhanced_->advisory)
            (void)match.advisory_navigation(s, g, flags, effective_navigation);
        const auto& advisory_navigation = effective_navigation;
        if (flow_head && !parent_index) {
            const bool had_head = flow_head->active;
            const auto previous_points = flow_head->points;
            const auto previous_count = flow_head->count;
            if (!g.navigation.goal || g.navigation.count < 2 ||
                !sim::ground_orders::route_present(g.navigation)) {
                flow_head->active = false;
                flow_head->count = 0;
                flow_head->mobile_blocked = false;
            } else {
                const auto goal_key = local_flow_goal_key(*g.navigation.goal);
                const bool retain_completed = match.rules().setup.enhanced_mode.enabled &&
                                              data::enhanced_mode::uses_retained_passing_flow(
                                                  match.rules().setup.enhanced_mode.version
                                              );
                if (retain_completed && !flow_head->active &&
                    flow_head->count == sim::unit_movement::local_flow_completed_count &&
                    (flow_head->goal_key != goal_key ||
                     !sim::unit_movement::local_flow_completed_within_history(
                         *flow_head, match.simulation_.tick
                     )))
                    flow_head->count = 0;
                const bool recover = sim::unit_movement::local_flow_recovery_due(
                    *flow_head, match.simulation_.tick, s.unit_index
                );
                const bool completed_head =
                    flow_head->active && flow_head->goal_key == goal_key &&
                    sim::unit_movement::local_flow_head_reached(g.geometry, *flow_head);
                // Completed passing legs can bound later choices on the same goal.
                const bool recent_completion =
                    flow_head->goal_key == goal_key &&
                    (retain_completed ? sim::unit_movement::local_flow_completed_within_history(
                                            *flow_head, match.simulation_.tick
                                        )
                                      : sim::unit_movement::local_flow_completed_last_tick(
                                            *flow_head, match.simulation_.tick
                                        ));
                if (flow_head->active && (flow_head->goal_key != goal_key || completed_head ||
                                          (recover && !advanced_corner))) {
                    flow_head->active = false;
                    flow_head->count =
                        completed_head ? sim::unit_movement::local_flow_completed_count : 0;
                    if (completed_head)
                        flow_head->tick = match.simulation_.tick;
                }
                if (!flow_head->active &&
                    (flow_head->blocked_ticks < sim::unit_movement::local_flow_retry_ticks ||
                     recover)) {
                    struct ProbeContext {
                        Match* match{};
                        sim::spatial_state::CellClassifier movement{};
                        uint32_t player{};
                        bool have_movement{};
                        bool same_route_queue{};
                        bool follow_applied_heads{};
                    } context;

                    context.match = &match;
                    context.player = s.record.owner_index;
                    context.follow_applied_heads = match.rules().setup.enhanced_mode.enabled &&
                                                   data::enhanced_mode::uses_following_flow(
                                                       match.rules().setup.enhanced_mode.version
                                                   );
                    context.same_route_queue =
                        (match.rules().setup.enhanced_mode.version ==
                             static_cast<int32_t>(data::enhanced_mode::battle_flow_version) ||
                         data::enhanced_mode::uses_distributed_battle_flow(
                             match.rules().setup.enhanced_mode.version
                         )) &&
                        flow_head->blocked_ticks < sim::unit_movement::local_flow_retry_ticks &&
                        sim::simulation_state::locally_simulated(u);
                    if (const auto* klass = match.movement_class_map(g.movement_class)) {
                        context.movement = klass->record;
                        context.movement.occupancy_before_tick = 0;
                        context.have_movement = true;
                    }
                    bool guide_ready = false;
                    if (data::enhanced_mode::uses_distributed_battle_flow(
                            match.rules().setup.enhanced_mode.version
                        ) &&
                        (uint64_t(match.simulation_.tick) + s.unit_index) %
                                local_throat_probe_period ==
                            0 &&
                        sim::simulation_state::locally_simulated(u) &&
                        (g.movement.flags & sim::unit_movement::occupancy_mask) ==
                            ground_occupancy &&
                        context.have_movement && context.movement.footprint_x == 2 &&
                        context.movement.footprint_z == 2 &&
                        g.geometry.footprint == sim::ground_orders::CorridorOrigin{2, 2}) {
                        const auto* goal = g.navigation.goal;
                        const auto* own = goal->order ? match.runtime_order(goal->order) : nullptr;
                        if (own && own->unit == &u && own->extra.goal.get() == goal &&
                            u.primary == goal->order) {
                            const sim::ground_orders::CorridorSampler sampler{
                                &context,
                                [](void* context,
                                   sim::ground_orders::CorridorOrigin cell,
                                   sim::ground_orders::CorridorOrigin footprint) {
                                    const auto& probe_context =
                                        *static_cast<ProbeContext*>(context);
                                    const auto& current = *probe_context.match;
                                    if (footprint != sim::ground_orders::CorridorOrigin{2, 2} ||
                                        probe_context.player >= OA_PLAYER_COUNT || cell[0] < 0 ||
                                        cell[1] < 0 ||
                                        uint32_t(cell[0]) + uint32_t(footprint[0]) >=
                                            current.spatial_.terrain_width ||
                                        uint32_t(cell[1]) + uint32_t(footprint[1]) >=
                                            current.spatial_.terrain_height)
                                        return sim::ground_orders::CorridorCell::blocked;
                                    const auto width = current.spatial_.terrain_width >> 1;
                                    const auto height = current.spatial_.terrain_height >> 1;
                                    for (int32_t z = 0; z < footprint[1]; ++z)
                                        for (int32_t x = 0; x < footprint[0]; ++x) {
                                            const auto sx = uint32_t(cell[0] + x) >> 1;
                                            const auto sz = uint32_t(cell[1] + z) >> 1;
                                            const auto at = size_t(sz) * width + sx;
                                            if (sx >= width || sz >= height ||
                                                at >= current.sight_.player_bits.size() ||
                                                (current.sight_.player_bits[at] &
                                                 (1u << probe_context.player)) == 0)
                                                return sim::ground_orders::CorridorCell::unknown;
                                        }
                                    return sim::ground_orders::static_flow_passable(
                                               probe_context.movement,
                                               cell[0],
                                               cell[1],
                                               current.spatial_
                                           )
                                               ? sim::ground_orders::CorridorCell::clear
                                               : sim::ground_orders::CorridorCell::blocked;
                                }
                            };
                            sim::ground_orders::CorridorBudget budget{
                                local_throat_maximum_probes, 0
                            };
                            bool consume_first = false;
                            guide_ready = try_local_throat_guide(
                                g.geometry,
                                g.navigation,
                                sampler,
                                budget,
                                effective_navigation,
                                &consume_first
                            );
                            if (guide_ready && consume_first) {
                                sim::ground_orders::advance_path(g.navigation, 1);
                                g.navigation.points[0] = local_flow_point(g.geometry);
                            }
                            match.enhanced_->local_flow_counters.probes += budget.used;
                        }
                    }
                    sim::unit_movement::LocalFlowProbe probe{};
                    probe.context = &context;
                    probe.allow_reverse = recover;
                    if (completed_head || recent_completion)
                        probe.previous_head = flow_head;
                    if (match.enhanced_->local_flow_progress_snapshot_valid &&
                        match.enhanced_->local_flow_progress_snapshot_tick ==
                            match.simulation_.tick)
                        probe.progress_at_tick_start = {
                            match.enhanced_->local_flow_progress_at_tick_start.get(),
                            match.enhanced_->unit_count
                        };
                    const auto own_progress = s.unit_index < probe.progress_at_tick_start.size()
                                                  ? probe.progress_at_tick_start[s.unit_index]
                                                  : g.movement.last_motion_tick;
                    if (retain_completed)
                        probe.allow_reverse =
                            recover && sim::unit_movement::local_flow_stationary_history_due(
                                           match.simulation_.tick, own_progress
                                       );
                    probe.recovery_yield =
                        recover && match.simulation_.tick - own_progress >=
                                       sim::unit_movement::local_flow_retry_ticks;
                    probe.at = [](void* context,
                                  const sim::unit_movement::Unit& moving,
                                  std::array<int16_t, 2> cell,
                                  uint8_t occupancy) {
                        const auto& probe_context = *static_cast<ProbeContext*>(context);
                        auto& m = *probe_context.match;
                        using sim::unit_movement::LocalFlowBlocker;
                        using sim::unit_movement::LocalFlowObstacle;
                        LocalFlowBlocker result{};
                        bool all_share_route = probe_context.same_route_queue;
                        if (occupancy != 1 || cell[0] < 0 || cell[1] < 0 ||
                            uint32_t(cell[0]) + uint32_t(moving.footprint[0]) >=
                                m.spatial_.terrain_width ||
                            uint32_t(cell[1]) + uint32_t(moving.footprint[1]) >=
                                m.spatial_.terrain_height)
                            return LocalFlowBlocker{LocalFlowObstacle::stationary};
                        // Classify visible terrain before looking for traffic
                        // beyond it. Unknown cells retain the normal fog rule.
                        if (probe_context.have_movement && probe_context.player < OA_PLAYER_COUNT) {
                            const auto width = m.spatial_.terrain_width >> 1;
                            const auto height = m.spatial_.terrain_height >> 1;
                            const auto x = uint32_t(cell[0] >> 1) +
                                           uint32_t(probe_context.movement.footprint_x >> 2);
                            const auto z = uint32_t(cell[1] >> 1) +
                                           uint32_t(probe_context.movement.footprint_z >> 2);
                            const auto index = size_t(z) * width + x;
                            if (x < width && z < height && index < m.sight_.player_bits.size() &&
                                (m.sight_.player_bits[index] & (1u << probe_context.player)) != 0 &&
                                !sim::ground_orders::static_flow_passable(
                                    probe_context.movement, cell[0], cell[1], m.spatial_
                                ))
                                return LocalFlowBlocker{LocalFlowObstacle::stationary};
                        }
                        // Small footprints and eight route samples bound this
                        // work independently of the match's unit count.
                        for (int32_t z = 0; z < moving.footprint[1]; ++z)
                            for (int32_t x = 0; x < moving.footprint[0]; ++x) {
                                const auto index = size_t(cell[1] + z) * m.spatial_.terrain_width +
                                                   size_t(cell[0] + x);
                                if (index >= m.spatial_.plots.size() ||
                                    m.spatial_.plots[index].blocking_feature)
                                    return LocalFlowBlocker{LocalFlowObstacle::stationary};
                                const auto occupant = m.spatial_.plots[index].ground;
                                if (occupant == sim::spatial_state::no_unit ||
                                    occupant == uint16_t(moving.id))
                                    continue;
                                const auto* other = m.ground_runtime(occupant);
                                if (!other || m.air_drivers_[occupant].unit ||
                                    other->geometry.type.maximum_speed <= 0 ||
                                    link_parent(m.slots_[occupant].record))
                                    return LocalFlowBlocker{LocalFlowObstacle::stationary};
                                if (all_share_route) {
                                    const auto follower_id = static_cast<uint16_t>(moving.id);
                                    const auto* follower = m.ground_runtime(follower_id);
                                    const auto owns_route = [&m](uint16_t id, const auto& ground) {
                                        const auto& slot = m.slots_[id];
                                        const auto* goal = ground.navigation.goal;
                                        if (!slot.unit || !goal || !goal->order ||
                                            !oa::unit_is_live_target(slot.record.flags) ||
                                            slot.record.health <= 0 ||
                                            slot.record.attach_parent != 0 ||
                                            slot.unit->primary != goal->order ||
                                            !sim::simulation_state::locally_simulated(*slot.unit))
                                            return false;
                                        const auto* own = m.runtime_order(goal->order);
                                        constexpr uint32_t cancel_event = 2;
                                        constexpr uint32_t terminal_events =
                                            sim::ground_orders::arrived_event |
                                            sim::ground_orders::path_failed_event |
                                            sim::ground_orders::goal_replaced_event | cancel_event;
                                        return own && own->unit == slot.unit &&
                                               own->extra.goal.get() == goal &&
                                               (goal->order->raised_events & terminal_events) ==
                                                   0 &&
                                               !sim::ground_orders::goal_contains_unit(
                                                   *goal, ground.geometry
                                               );
                                    };
                                    all_share_route =
                                        follower &&
                                        m.slots_[occupant].record.owner_index ==
                                            probe_context.player &&
                                        !other->mirrored_driver &&
                                        (other->movement.flags &
                                         sim::unit_movement::occupancy_mask) == ground_occupancy &&
                                        same_forward_route(
                                            moving,
                                            follower->navigation,
                                            other->geometry,
                                            other->navigation
                                        ) &&
                                        owns_route(follower_id, *follower) &&
                                        owns_route(occupant, *other);
                                    if (all_share_route && probe_context.follow_applied_heads) {
                                        const auto* applied =
                                            occupant < m.enhanced_->unit_count
                                                ? &m.enhanced_->local_flow_heads[occupant]
                                                : nullptr;
                                        all_share_route = same_forward_route_with_applied_head(
                                            moving,
                                            follower->navigation,
                                            other->geometry,
                                            other->navigation,
                                            applied,
                                            local_flow_goal_key(*other->navigation.goal),
                                            m.simulation_.tick,
                                            true
                                        );
                                    }
                                }
                                const bool have_snapshot =
                                    m.enhanced_->local_flow_progress_snapshot_valid &&
                                    m.enhanced_->local_flow_progress_snapshot_tick ==
                                        m.simulation_.tick;
                                const auto last_progress =
                                    have_snapshot
                                        ? m.enhanced_->local_flow_progress_at_tick_start[occupant]
                                        : other->movement.last_motion_tick;
                                const auto waiting = m.simulation_.tick - last_progress;
                                if (result.obstacle != LocalFlowObstacle::moving ||
                                    waiting > result.waiting_ticks ||
                                    (waiting == result.waiting_ticks && occupant < result.unit_id))
                                    result = {
                                        LocalFlowObstacle::moving,
                                        occupant,
                                        other->geometry.heading,
                                        waiting
                                    };
                            }
                        result.same_route_queue =
                            result.obstacle == LocalFlowObstacle::moving && all_share_route;
                        return result;
                    };
                    const auto points = sim::ground_orders::steering_points(advisory_navigation);
                    const auto target = sim::ground_orders::steering_target(g.geometry, points);
                    const auto decision = sim::unit_movement::choose_local_flow(
                        g.geometry,
                        g.movement,
                        points[1],
                        target.point,
                        match.simulation_.tick,
                        *this,
                        probe,
                        match.enhanced_->local_flow_counters
                    );
                    flow_head->mobile_blocked = decision.mobile_blocked;
                    if (decision.adjusted) {
                        flow_head->points = {
                            local_flow_point(g.geometry),
                            sim::ground_orders::RoutePoint{
                                static_cast<int16_t>(decision.target[0] >> 16),
                                static_cast<int16_t>(decision.target[2] >> 16)
                            },
                            sim::ground_orders::RoutePoint{
                                static_cast<int16_t>(points[2][0] >> 16),
                                static_cast<int16_t>(points[2][2] >> 16)
                            }
                        };
                        flow_head->count = 3;
                        flow_head->active = true;
                        flow_head->goal_key = goal_key;
                    } else if (guide_ready && !flow_head->active) {
                        std::copy_n(
                            effective_navigation.points.begin(), 3, flow_head->points.begin()
                        );
                        flow_head->count = 3;
                        flow_head->goal_key = goal_key;
                        flow_head->tick = match.simulation_.tick;
                        flow_head->active = true;
                    }
                }
            }
            if (flow_head->active)
                flow_head->tick = match.simulation_.tick;
            if (had_head != flow_head->active ||
                (flow_head->active &&
                 (previous_points != flow_head->points || previous_count != flow_head->count)))
                g.navigation.flags |= sim::ground_orders::route_changed_flag;
            if (flow_head->active) {
                std::copy(
                    flow_head->points.begin(),
                    flow_head->points.end(),
                    effective_navigation.points.begin()
                );
                effective_navigation.count = flow_head->count;
                effective_navigation.flags |= sim::ground_orders::route_present_flag;
            }
        }
        sim::ground_orders::steer_ground(
            g.geometry,
            g.movement,
            effective_navigation,
            g.acceleration,
            g.deceleration,
            match.simulation_.sea_level,
            fast_unit_steering()
        );
    }
    if (parent_index) {
        auto& parent = match.slots_.at(parent_index);
        const auto piece = std::bit_cast<int8_t>(s.record.attach_piece);
        std::array<uint32_t, 3> world = parent.unit ? parent.unit->position : u.position;
        if (piece >= 0)
            world = match.piece_world_position(parent, static_cast<uint32_t>(piece));
        std::array<sim::unit_movement::Fixed, 3> next{
            signed_word(world[0]), signed_word(world[1]), signed_word(world[2])
        };
        if (u.type && (u.type->flags & floater_type_flag) != 0) {
            const auto water = static_cast<sim::unit_movement::Fixed>(
                (static_cast<uint32_t>(u.type->waterline_offset) * 0xffffu +
                 match.simulation_.sea_level)
                << 16
            );
            if (!(water < next[1]))
                next[1] = water;
        }
        // The carried unit's cell, bucket and sight follow the piece; an
        // attached unit stays out of the bucket chains.
        match.place_unit(s, next[0], next[1], next[2], g.movement.flags & 3);
        g.project_slot();
        if (piece >= 0) {
            if (auto* inst = match.instance(parent.unit_index)) {
                const auto index = static_cast<uint32_t>(piece);
                if (index < inst->model().pieces().size()) {
                    const auto attitude = inst->piece_attitude(index);
                    s.record.bank = attitude.xy;
                    s.yaw = static_cast<uint16_t>(attitude.xz);
                    s.record.pitch = attitude.yz;
                }
            }
        } else {
            s.record.bank = parent.record.bank;
            s.yaw = parent.yaw;
            s.record.pitch = parent.record.pitch;
        }
        g.geometry.heading = s.yaw;
        g.geometry.pitch = s.record.pitch;
        if (auto* parent_ground = match.ground_runtime(parent.unit_index)) {
            g.movement.velocity = parent_ground->movement.velocity;
            g.movement.speed = parent_ground->movement.speed;
        } else {
            g.movement.velocity = {};
            g.movement.speed = 0;
        }
        g.geometry.flags &= ~sim::unit_movement::position_dirty;
    } else {
        if (enhanced_flow && !air_driven && !g.mirrored_driver &&
            match.rules().setup.enhanced_mode.version >=
                static_cast<int32_t>(data::enhanced_mode::swept_ground_version))
            (void)sim::unit_movement::integrate_swept_local_flow(
                g.geometry,
                g.movement,
                match.simulation_.tick,
                sim::simulation_state::locally_simulated(u),
                *this,
                match.enhanced_->local_flow_counters
            );
        else if (enhanced_flow && !air_driven)
            sim::unit_movement::integrate_local_flow(
                g.geometry,
                g.movement,
                match.simulation_.tick,
                sim::simulation_state::locally_simulated(u),
                *this,
                match.enhanced_->local_flow_counters
            );
        else
            sim::unit_movement::integrate_unattached(
                g.geometry,
                g.movement,
                match.simulation_.tick,
                sim::simulation_state::locally_simulated(u),
                *this
            );
        if (flow_head && !air_driven && !g.mirrored_driver)
            sim::unit_movement::record_local_flow_block(
                *flow_head, (g.movement.flags & sim::unit_movement::collision_blocked) != 0
            );
    }
    g.write_slot();
    write_flags(s, flags);
    signal_move_rate(s, g.movement);
    // The sea occupy code the script last heard lives in Unit.last_occupy_code;
    // setSFXoccupy runs only when it changes.
    sim::world_environment::SeaOccupyHost sea_host{};
    sea_host.context = &match;
    sea_host.reordered = match.rules().units.water_state_rules.rules ==
                         data::match_rules::UnitsWaterStateRulesRules::reordered;
    sea_host.set_sfx_occupy = [](void* context, oa::Unit& unit, int32_t occupy_code) {
        auto* object = static_cast<Match*>(context)->instance(unit.id);
        if (object && object->script())
            object->script()->call(
                ScriptFunction::set_sfx_occupy, std::span(&occupy_code, 1), true
            );
    };
    sim::world_environment::update_sea_occupy(
        s.record, match_unit_def(match, s.record), match.simulation_.sea_level, sea_host
    );
}

void TickHost::signal_move_rate(
    sim::unit_spawn::Slot& s, const sim::unit_movement::Movement& movement
) {
    const auto rate = movement_rate(movement, s.record, match_unit_def(match, s.record));
    auto& unit_flags = s.record.flags;
    if (rate == ((unit_flags & moving_rate_mask) >> moving_rate_shift))
        return;
    if (rate == 0)
        script(s, ScriptFunction::stop_moving);
    else if ((unit_flags & moving_rate_mask) == 0)
        script(s, ScriptFunction::start_moving);
    if (rate == 1)
        script(s, ScriptFunction::move_rate1);
    else if (rate == 2)
        script(s, ScriptFunction::move_rate2);
    else if (rate == 3)
        script(s, ScriptFunction::move_rate3);
    unit_flags = (unit_flags & ~moving_rate_mask) | (rate << moving_rate_shift);
}

bool TickHost::can_occupy(
    const sim::unit_movement::Unit& u, std::array<int16_t, 2> cell, uint8_t mode
) {
    auto& s = movement_slot(u);
    if (!match.fields(s).runtime_metadata || !match.collision_terrain_) {
        match.fault_.note("moving collision requires resolved type and terrain metadata");
        return false;
    }
    // The test reads only this unit's projection and the plots.
    auto& projected = match.project_spatial(s);
    const auto result =
        sim::spatial_state::can_occupy(projected, s.unit_index, cell, mode, match.spatial_);
    if (!result)
        return false;
    return *result;
}

void TickHost::remove_occupancy(sim::unit_movement::Unit& u) {
    auto& s = movement_slot(u);
    ground(*s.unit).write_slot();
    auto& projected = match.begin_spatial_change(s);
    match.spatial_.tick = match.simulation_.tick;
    const auto result =
        sim::spatial_state::remove_occupancy(projected, match.spatial_, match.map_listeners_);
    match.end_spatial_change(true);
    ground(*s.unit).project_slot();
    if (result != sim::spatial_state::Error::none)
        match.fault_.note("moving occupancy removal rejected spatial state");
}

void TickHost::insert_occupancy(sim::unit_movement::Unit& u) {
    auto& s = movement_slot(u);
    ground(*s.unit).write_slot();
    match.register_occupancy(s);
    ground(*s.unit).project_slot();
}

void TickHost::update_spatial_membership(sim::unit_movement::Unit& u) {
    auto& s = movement_slot(u);
    ground(*s.unit).write_slot();
    match.update_moving_sight(s);
    ground(*s.unit).project_slot();
}

void Match::step_mirrored_movement(uint16_t unit) {
    auto& s = slots_.at(unit);
    if (!s.unit || !ground_runtime(unit))
        return;
    TickHost host(*this);
    host.movement_tick(s.record);
}

void Match::refresh_mirrored_height(uint16_t unit) {
    auto& s = slots_.at(unit);
    if (!s.unit || !ground_runtime(unit))
        return;
    TickHost host(*this);
    sim::simulation_state::update_height(state(), s.record, host);
}

void Match::set_mirrored_air_goal(uint16_t unit, const sim::air::AirGoal* goal) {
    auto& driver = air_drivers_.at(unit);
    if (driver.unit == nullptr || driver.local)
        return;
    auto& owned = mirrored_air_goals_.at(unit);
    owned = goal != nullptr ? *goal : sim::air::AirGoal{};
    driver.goal = goal != nullptr ? &owned : nullptr;
}

void Match::set_movement_layer(uint16_t unit, uint8_t layer) {
    auto& s = slots_.at(unit);
    auto* g = ground_runtime(unit);
    if (!s.unit || !g)
        return;
    g->project_slot();
    set_movement_layer(s, *g, layer);
}

void Match::set_movement_layer(
    sim::unit_spawn::Slot& s, sim::ground_orders::GroundRuntime& g, uint8_t layer
) {
    if ((g.movement.flags & sim::unit_movement::occupancy_mask) == layer)
        return;
    if (layer == sim::air::layer_ground) {
        g.movement.velocity = {0, 0, 0};
        g.movement.speed = 0;
        const oa::UnitDef* def = oa::world_unit_def_of(&state(), &s.record);
        int16_t bank = 0;
        sim::air::air_attitude(
            g.geometry,
            g.previous_vector,
            {0, 0, 0},
            def != nullptr ? def->bank_scale : 0,
            def != nullptr ? def->pitch_scale : 0,
            state().game.gravity != 0 ? state().game.gravity : sim::ground_orders::default_gravity,
            &bank
        );
        s.record.bank = bank;
        s.record.pitch = g.geometry.pitch;
        set_activation(s, sim::unit_activation::active_mask, false);
    } else
        set_activation(s, sim::unit_activation::active_mask, true);
    g.movement.flags = static_cast<uint8_t>(
        ((g.movement.flags ^ layer) & sim::unit_movement::occupancy_mask) ^ g.movement.flags
    );
}

} // namespace oa::sim::match_runtime
