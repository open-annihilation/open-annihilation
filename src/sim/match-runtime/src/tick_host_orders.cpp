// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "tick_internal.hpp"

#include <cstdint>

namespace oa::sim::match_runtime {

namespace {
// Mission handler result on which the dispatcher clears all of the unit's orders.
constexpr uint32_t mission_invalid = 7;
// Order events of an installed goal: arrived, failed, replaced.
constexpr uint32_t goal_events = sim::ground_orders::arrived_event |
                                 sim::ground_orders::path_failed_event |
                                 sim::ground_orders::goal_replaced_event;
// Weapon event; the queue re-checks every weapon slot when it wakes an order.
constexpr uint32_t weapon_wake_event = 0x10000;
// Speech category VTOL_GetRepaired announces once the aircraft is repaired;
// its other announcement is the failure of a lost pad.
constexpr uint32_t vtol_get_repaired_repaired_speech = 10;
} // namespace

void TickHost::take_off(sim::unit_spawn::Slot& s, sim::simulation_state::Order& order) {
    before_callback();
    match.release_tracked_weapons(s, 3);
    after_callback();
    if (s.record.attach_parent)
        match.set_carry_link(s.unit_index, 0, -1, 2);
    match.set_activation(s, 1, true);
    auto* g = match.ground_runtime(s.unit_index);
    if (!g)
        return;
    g->project_slot();
    if ((g->movement.flags & 3) != sim::air::layer_ground)
        return;
    match.set_movement_layer(s, *g, sim::air::layer_air);
    auto goal = sim::air::air_goal_at_point(&order.raised_events, &s.record, s.record.position);
    sim::air::air_goal_set_altitude(
        &goal, air_host(), static_cast<int16_t>(match_unit_def(match, s.record).cruise_alt / 2)
    );
    install_air_goal(s, order, &goal);
    order.wait_events |= goal_events;
}

void TickHost::queue_default_mission(oa::World&, oa::Unit& record) {
    auto& u = unit_view(record);
    const auto kind = static_cast<uint8_t>(match_unit_def(match, record).default_mission_type);
    if (!kind)
        return;
    if (!match.ground_runtime(slot(u).unit_index) && kind != get_built_kind)
        return;
    // Skip DefaultMissionType kinds whose handlers the match does not run yet.
    if (kind != sim::ground_orders::standby_kind && kind != sim::ground_orders::move_ground_kind &&
        kind != sim::ground_orders::vtol_move_kind && kind != get_built_kind &&
        kind != patrol_kind && kind != follow_ground_kind &&
        kind != sim::ground_orders::vtol_standby_kind &&
        kind != sim::ground_orders::vtol_land_if_can_kind)
        return;
    auto& order = match.insert_ground_order(slot(u).unit_index, kind);
    owned(order).extra.command_flags |= command_overlay;
    // A frame waiting for its builder is not idle, and GetBuilt is that wait.
    if (record.build_remaining == 0.0F && kind != get_built_kind) {
        const auto unit_slot = static_cast<uint16_t>(oa::world_unit_slot(&match.state(), &record));
        match.emit_event<&EventHooks::unit_idle>(match.state(), unit_slot);
    }
}

void Match::report_order_end(
    void* match, const oa::Unit& unit, const sim::simulation_state::Order& order, bool done
) {
    auto& self = *static_cast<Match*>(match);
    const auto unit_slot = static_cast<uint16_t>(oa::world_unit_slot(&self.state(), &unit));
    self.emit_event<&EventHooks::order_ended>(self.state(), unit_slot, order.kind, done);
}

uint32_t TickHost::dispatch_mission(
    oa::World&, oa::Unit& record, sim::simulation_state::Order& order, uint32_t events
) {
    auto& world = match.simulation_;
    auto& u = unit_view(record);
    auto& s = slot(u);
    uint32_t result;
    if (dispatch_ground_mission(s, order, events, result) ||
        dispatch_vtol_mission(s, order, events, result) ||
        dispatch_vtol_build_mission(s, order, events, result) ||
        dispatch_air_attack_mission(s, order, events, result))
        return result;
    auto flags = weapon_flags(s);
    if (order.kind == 0) {
        write_flags(s, flags);
        return blank_mission_step();
    }
    if (order.kind == vtol_get_repaired_kind) {
        // VTOL_GetRepaired does not use a ground controller.
        const auto& order_record = owned(order);
        if (!u.type && order.phase == 0 &&
            (order_record.construction.target != nullptr ||
             order_record.attack.target != nullptr)) {
            match.fault_.note("VTOL_GetRepaired unit has no type");
            write_flags(s, flags);
            return mission_fault_result;
        }
        const auto step = vtol_get_repaired(
            order_record.construction.target != nullptr || order_record.attack.target != nullptr,
            order.phase,
            u.health,
            u.type ? u.type->maximum_health : 0
        );
        if (step.announce != 0)
            play_sound(
                u,
                step.announce,
                step.announce == vtol_get_repaired_repaired_speech ? "Unit repaired"
                                                                   : "Repair aborted."
            );
        if (step.wait) {
            order.wait_events |= 1;
            order.wake_tick = world.tick + 0x1e;
            order.wait_events |= 8;
        }
        write_flags(s, flags);
        return step.result;
    }
    // Only the steering handlers use the movement object (Unit.movement); a
    // structure (bmcode 0) has none, yet it attacks, holds fire and is paralysed.
    auto* movement = match.ground_runtime(s.unit_index);
    if (movement)
        movement->project_slot();
    const auto steering = [&] { return view(s, ground(u), flags); };

    struct DispatchScope {
        TickHost& host;
        std::array<uint8_t, 3>* previous_flags;
        sim::unit_spawn::Slot* previous_slot;

        ~DispatchScope() {
            host.dispatched_flags = previous_flags;
            host.dispatched_slot = previous_slot;
        }
    } scope{*this, dispatched_flags, dispatched_slot};

    dispatched_flags = &flags;
    dispatched_slot = &s;
    if (order.kind == sim::ground_orders::standby_kind)
        // Standby's phase 0 ends the order when Unit.movement is null.
        result = !movement && order.phase == 0
                     ? mission_invalid
                     : sim::ground_orders::standby(steering(), order, world.tick, *this);
    else if (order.kind == sim::ground_orders::vtol_standby_kind) {
        // VTOL_Standby. Phase 0 enables weapons and waits one tick. Phase 1
        // orders an attack on the automatic target and restarts, or returns 1.
        if (!s.unit->object_present || !s.unit->type ||
            (s.unit->type->flags & OA_UNIT_DEF_FLAG_CAN_FLY) == 0)
            result = 7;
        else if (order.phase == 0) {
            release_tracked_weapons(s, flags);
            order.wait_events |= weapon_wake_event | 1;
            order.wake_tick = world.tick + 1;
            auto& saved = owned(order).extra;
            saved.anchor_x = static_cast<int16_t>(s.unit->position[0] >> 16);
            saved.anchor_z = static_cast<int16_t>(s.unit->position[2] >> 16);
            result = 1;
        } else if (order.phase == 1) {
            if (auto* target = match.find_automatic_target(*s.unit);
                target && match.issue_automatic_attack(*s.unit, *target)) {
                order.phase = 0;
                result = 3;
            } else
                result = 1;
        } else if (order.phase == 2) {
            auto& projected = match.project_spatial(s);
            const bool airborne = (s.unit->flags & 3u) == 2;
            if ((s.unit->type->flags & OA_UNIT_DEF_FLAG_CAN_FLY) == 0 || !airborne) {
                order.phase = 1;
                order.wait_events |= weapon_wake_event | 1;
                order.wake_tick = world.tick + random(0x1e) + 0x1e;
                result = 2;
            } else if (projected.first_attachment == 0) {
                (void)match.insert_ground_order(
                    s.unit_index, sim::ground_orders::vtol_land_if_can_kind
                );
                result = 5;
            } else {
                // A loaded transport circles its anchor at cruise altitude.
                const auto heading = static_cast<uint16_t>(random(0x10000));
                const auto radius = static_cast<sim::unit_movement::Fixed>(
                    (static_cast<int32_t>(random(0x20)) + 8) * 0x10000
                );
                const auto& saved = owned(order).extra;
                const oa::FixedVec3 circle{
                    (static_cast<int32_t>(saved.anchor_x) << 16) -
                        sim::unit_movement::sine_scaled(heading, radius),
                    0,
                    (static_cast<int32_t>(saved.anchor_z) << 16) -
                        sim::unit_movement::cosine_scaled(heading, radius)
                };
                auto goal = sim::air::air_goal_at_point(&order.raised_events, &s.record, circle);
                sim::air::air_goal_set_altitude(
                    &goal, air_host(), match_unit_def(match, s.record).cruise_alt
                );
                install_air_goal(s, order, &goal);
                order.phase = 1;
                order.wait_events |= 1;
                order.wake_tick = world.tick + random(0xf) + 0x1e;
                result = 2;
            }
        } else
            result = 7;
    } else if (order.kind == sim::ground_orders::move_ground_kind)
        result = sim::ground_orders::move_ground(
            steering(), order, owned(order).extra, events, world.tick, *this
        );
    else if (order.kind == paralyze_order_kind) {
        // Paralyze. The first parameter is the duration.
        struct Paralyzed {
            TickHost& host;
            sim::unit_spawn::Slot& slot;
            Match::RuntimeOrder& record;
            std::array<uint8_t, 3>& flags;
        } paralyzed{*this, s, owned(order), flags};

        sim::unit_health::ParalysisHooks hooks;
        hooks.context = &paralyzed;
        hooks.target_cleared = [](void* context, oa::Unit& unit, uint8_t slot) {
            static_cast<Paralyzed*>(context)->host.match.target_cleared(unit, slot);
        };
        hooks.set_state_flags = [](void* context, oa::Unit&, uint8_t mask, bool on) {
            auto& p = *static_cast<Paralyzed*>(context);
            p.host.match.set_activation(p.slot, mask, on);
        };
        hooks.release_order_goal = [](void* context) {
            auto& p = *static_cast<Paralyzed*>(context);
            if (!p.record.extra.goal)
                return;
            sim::ground_orders::install_goal(
                p.host.view(p.slot, p.host.ground(*p.slot.unit), p.flags),
                nullptr,
                p.host.match.simulation_.tick,
                p.host
            );
            p.record.extra.goal.reset();
        };
        write_flags(s, flags);
        const auto step = sim::unit_health::paralyzed_mission_step(
            s.record, paralyzed.record.extra.tolerance, hooks
        );
        flags = weapon_flags(s);
        if (step.result == sim::unit_health::mission_result_waiting) {
            order.wait_events |= 1;
            order.wake_tick = world.tick + static_cast<uint32_t>(step.wait_ticks);
        }
        result = step.result;
    } else {
        match.fault_.note("mission kind outside the ground order handlers");
        result = mission_fault_result;
    }
    write_flags(s, flags);
    return result;
}

void TickHost::stop_building(sim::unit_spawn::Slot& s, sim::simulation_state::Order& order) {
    if ((order.flags & order_building_flag) == 0)
        return;
    auto* object = match.instance(s.unit_index);
    auto* script = object != nullptr ? object->script() : nullptr;
    const auto function = script != nullptr ? script->find("StopBuilding") : -1;
    if (script != nullptr)
        script->call("StopBuilding", {}, false);
    match.share_script_start(s.unit_index, static_cast<int16_t>(function));
    order.flags &= static_cast<uint8_t>(~order_building_flag);
}

void TickHost::destroy_order(oa::Unit& unit, sim::simulation_state::Order& order) {
    auto& s = slot(unit_view(unit));
    auto& record = owned(order);
    if (order.wait_events & 2)
        (void)dispatch_mission(match.state(), unit, order, 2);
    stop_building(s, order);
    if (auto* g = match.ground_runtime(s.unit_index); g && record.extra.goal) {
        if (g->navigation.goal == record.extra.goal.get()) {
            auto flags = weapon_flags(s);
            sim::ground_orders::install_goal(
                view(s, *g, flags), nullptr, match.simulation_.tick, *this
            );
            write_flags(s, flags);
        }
        record.extra.goal.reset();
    }
    // Tearing the goal down leaves the driver without a goal.
    if (auto& driver = match.air_drivers_.at(s.unit_index); driver.goal == &record.air_goal)
        sim::air::air_driver_set_goal(&driver, nullptr);
    record.air_goal = {};
    if (!(order.flags & 1))
        match.bridge_->reset_weapon_targets(s.record, 3);
    match.unlink_target_observer(record);
    std::erase_if(match.orders_, [&](const auto& candidate) { return candidate.get() == &record; });
}

} // namespace oa::sim::match_runtime
