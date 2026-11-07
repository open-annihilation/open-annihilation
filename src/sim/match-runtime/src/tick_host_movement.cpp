// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "tick_internal.hpp"
#include "enhanced_state.hpp"

#include "oa/sim/air/flight.hpp"
#include "oa/sim/air/host.hpp"
#include "oa/sim/ground_orders/steering_target.hpp"
#include "oa/sim/world_environment/wind.hpp"

#include <cstdint>

namespace oa::sim::match_runtime {
namespace {

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
        auto effective_navigation = g.navigation;
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
                const bool recover = sim::unit_movement::local_flow_recovery_due(
                    *flow_head, match.simulation_.tick, s.unit_index
                );
                const bool completed_head =
                    flow_head->active && flow_head->goal_key == goal_key &&
                    sim::unit_movement::local_flow_head_reached(g.geometry, *flow_head);
                // One tick on the original route can separate two local choices.
                const bool recent_completion = flow_head->goal_key == goal_key &&
                                               sim::unit_movement::local_flow_completed_last_tick(
                                                   *flow_head, match.simulation_.tick
                                               );
                if (flow_head->active &&
                    (flow_head->goal_key != goal_key || completed_head || recover)) {
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
                    } context;

                    context.match = &match;
                    context.player = s.record.owner_index;
                    if (const auto* klass = match.movement_class_map(g.movement_class)) {
                        context.movement = klass->record;
                        context.movement.occupancy_before_tick = 0;
                        context.have_movement = true;
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
                    probe.at = [](void* context,
                                  const sim::unit_movement::Unit& moving,
                                  std::array<int16_t, 2> cell,
                                  uint8_t occupancy) {
                        const auto& probe_context = *static_cast<ProbeContext*>(context);
                        auto& m = *probe_context.match;
                        using sim::unit_movement::LocalFlowBlocker;
                        using sim::unit_movement::LocalFlowObstacle;
                        LocalFlowBlocker result{};
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
                        return result;
                    };
                    const auto points = sim::ground_orders::steering_points(g.navigation);
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
        if (enhanced_flow && !air_driven)
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
