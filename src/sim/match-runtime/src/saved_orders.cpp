// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Saved orders: the order words a savegame keeps, taken from and given back
// to the runtime order records.
#include "tick_internal.hpp"

#include "oa/data/mission_types.hpp"

#include <cstdint>

namespace oa::sim::match_runtime {

namespace {

// Mission indices whose handlers keep their words in AttackOrderState or
// ConstructionOrderState and not in the ground OrderState.
constexpr uint8_t air_evade_kind = 48;      // VTOL_Evade
constexpr uint8_t air_seek_guard_kind = 63; // VTOL_SeekGuard
constexpr uint8_t attack_unit_type_kind = 10;
constexpr uint8_t build_weapon_kind = 13;
constexpr uint8_t guard_no_move_kind = 22;
constexpr uint8_t resurrect_kind = 37;
constexpr uint8_t vtol_reclaim_kind = 58;

// Order flags bit: the order lives on the secondary queue.
constexpr uint8_t secondary_queue_flag = 0x04;

// The runtime order keeps the order record's point, anchor and three
// parameter words once per handler family. A save takes each word from the
// copy the kind's handler keeps it in; a restore sets every copy, as order
// creation does.
enum class WordCopy : uint8_t { ground, attack, construction };

// The aircraft attack handlers.
bool air_attack(uint8_t kind) {
    switch (kind) {
    case air_strike_kind:
    case air_to_air_kind:
    case air_to_ground_kind:
    case air_to_ground_hover_kind:
    case air_evade_kind:
    case vtol_seek_attack_kind:
    case air_seek_guard_kind:
        return true;
    default:
        return false;
    }
}

/// Returns the copy that keeps an order's point.
///
/// @param kind Mission kind.
/// @return The family whose copy a save reads.
WordCopy point_copy(uint8_t kind) {
    return air_attack(kind) ? WordCopy::attack : WordCopy::ground;
}

/// Returns the copy that keeps an order's anchor, X and Z.
///
/// @param kind Mission kind.
/// @return The family whose copy a save reads.
WordCopy anchor_copy(uint8_t kind) {
    return air_attack(kind) || kind == attack_chase_kind ? WordCopy::attack : WordCopy::ground;
}

bool construction_type_word(uint8_t kind) {
    switch (kind) {
    case attack_unit_type_kind:
    case building_build_kind:
    case capture_kind:
    case help_build_kind:
    case mobile_build_kind:
    case reclaim_unit_kind:
    case repair_unit_kind:
    case repair_unit_no_move_kind:
    case resurrect_kind:
    case vtol_help_build_kind:
    case vtol_mobile_build_kind:
    case vtol_reclaim_kind:
    case vtol_reclaim_unit_kind:
    case vtol_repair_unit_kind:
        return true;
    default:
        return false;
    }
}

/// Returns the copy that keeps an order's first parameter.
///
/// @param kind Mission kind.
/// @return The family whose copy a save reads.
WordCopy parameter_1_copy(uint8_t kind) {
    if (construction_type_word(kind))
        return WordCopy::construction;
    switch (kind) {
    case attack_chase_kind:
    case attack_no_move_kind:
    case attack_special_kind:
    case build_weapon_kind:
    case guard_no_move_kind:
    case suppress_kind:
        return WordCopy::attack;
    default:
        return air_attack(kind) ? WordCopy::attack : WordCopy::ground;
    }
}

/// Returns the copy that keeps an order's second parameter.
///
/// @param kind Mission kind.
/// @return The family whose copy a save reads.
WordCopy parameter_2_copy(uint8_t kind) {
    return construction_type_word(kind) || kind == build_weapon_kind ? WordCopy::construction
                                                                     : WordCopy::attack;
}

/// Returns the copy that keeps an order's third parameter.
///
/// @param kind Mission kind.
/// @return The family whose copy a save reads.
WordCopy parameter_3_copy(uint8_t kind) {
    switch (kind) {
    case build_weapon_kind:
    case mobile_build_kind:
    case vtol_help_build_kind:
    case vtol_mobile_build_kind:
        return WordCopy::construction;
    default:
        return WordCopy::attack;
    }
}

int32_t pick(WordCopy copy, int32_t ground, int32_t attack, int32_t construction) {
    switch (copy) {
    case WordCopy::attack:
        return attack;
    case WordCopy::construction:
        return construction;
    case WordCopy::ground:
        break;
    }
    return ground;
}

bool live(const sim::simulation_state::Unit* unit) {
    return unit != nullptr && (unit->record.flags & OA_UNIT_FLAG_LIVE) != 0;
}

// The goal of a runtime order (Match::RuntimeOrder).
template <typename Record>
void save_goal(const Record& record, data::persist::SavedGoal& saved) {
    saved = {};
    if (const auto* goal = record.extra.goal.get()) {
        saved.kind = static_cast<int32_t>(goal->shape);
        switch (goal->shape) {
        case sim::ground_orders::GoalShape::circle:
            saved.circle = {{goal->cell[0], goal->cell[1]}, goal->tolerance, goal->radius_squared};
            break;
        case sim::ground_orders::GoalShape::ring:
            saved.ring = {
                {goal->cell[0], goal->cell[1]},
                goal->inner_range,
                goal->tolerance,
                goal->inner_radius_squared,
                goal->radius_squared
            };
            break;
        case sim::ground_orders::GoalShape::outline:
            saved.outline = {goal->left, goal->right, goal->top, goal->bottom};
            break;
        }
        return;
    }
    const auto& air_goal = record.air_goal;
    const auto id = [](const oa::Unit* unit) { return unit != nullptr ? unit->id : uint16_t{0}; };
    switch (air_goal.kind) {
    case sim::air::AirGoalKind::target:
        saved.kind = static_cast<int32_t>(data::persist::SavedGoalKind::air_target);
        saved.air_target.unit_id = id(air_goal.unit);
        saved.air_target.target_id = id(air_goal.target);
        saved.air_target.flags = air_goal.flags;
        saved.air_target.arrival_radius = air_goal.arrival_radius;
        saved.air_target.altitude = air_goal.altitude;
        saved.air_target.bearing = air_goal.bearing;
        saved.air_target.query_point = air_goal.query_point;
        saved.air_target.point[0] = air_goal.point.x;
        saved.air_target.point[1] = air_goal.point.y;
        saved.air_target.point[2] = air_goal.point.z;
        saved.air_target.stand_off = air_goal.stand_off;
        break;
    case sim::air::AirGoalKind::seek:
        // The runtime goal keeps neither the word before the heading, which
        // starts zero, nor the word after it, which nothing sets; both are
        // saved as zero.
        saved.kind = static_cast<int32_t>(data::persist::SavedGoalKind::air_seek);
        saved.air_seek.unit_id = id(air_goal.unit);
        saved.air_seek.flags = air_goal.flags;
        saved.air_seek.point[0] = air_goal.point.x;
        saved.air_seek.point[1] = air_goal.point.y;
        saved.air_seek.point[2] = air_goal.point.z;
        saved.air_seek.step[0] = air_goal.step.x;
        saved.air_seek.step[1] = air_goal.step.y;
        saved.air_seek.step[2] = air_goal.step.z;
        saved.air_seek.heading = air_goal.heading;
        break;
    case sim::air::AirGoalKind::none:
        break;
    }
}

} // namespace

void Match::visit_saved_orders(
    uint16_t index, data::persist::SavedOrderVisit visit, void* walk
) const {
    if (visit == nullptr || index >= units_.size())
        return;
    const auto& unit = units_[index];
    for (auto* head : {unit.primary, unit.secondary}) {
        for_each_primary_uncycled(head, [&](sim::simulation_state::Order* order) {
            const auto* record = runtime_order(order);
            if (record == nullptr)
                return;
            const auto kind = order->kind;
            data::persist::SavedOrder saved;
            saved.owner_id = unit.record.id;
            const auto* target =
                record->attack.target ? record->attack.target : record->construction.target;
            saved.target_id = live(target) ? target->record.id : uint16_t{0};
            saved.kind = kind;
            saved.phase = order->phase;
            saved.wait_events = order->wait_events;
            saved.wake_tick = order->wake_tick;
            for (std::size_t axis = 0; axis < 3; ++axis)
                saved.point[axis] = point_copy(kind) == WordCopy::attack
                                        ? std::bit_cast<int32_t>(record->attack.destination[axis])
                                        : record->extra.destination[axis];
            if (anchor_copy(kind) == WordCopy::attack) {
                saved.anchor[0] = record->attack.origin[0];
                saved.anchor[1] = record->attack.origin[1];
            } else {
                saved.anchor[0] = record->extra.anchor_x;
                saved.anchor[1] = record->extra.anchor_z;
            }
            saved.parameter_1 = pick(
                parameter_1_copy(kind),
                record->extra.tolerance,
                record->attack.weapon_slot,
                record->construction.type_index
            );
            saved.parameter_2 = pick(
                parameter_2_copy(kind), 0, record->attack.retry, record->construction.remaining
            );
            saved.parameter_3 = pick(
                parameter_3_copy(kind),
                0,
                record->attack.leash,
                record->construction.blocked_retries
            );
            saved.seen_cell = static_cast<uint16_t>(order->seen_x) |
                              static_cast<uint32_t>(static_cast<uint16_t>(order->seen_z)) << 16;
            saved.preserve_flags = order->preserve_flags;
            saved.command_flags = record->extra.command_flags;
            saved.flags = order->flags;
            saved.mission_byte = oa::data::mission_types::mission_flags(kind);
            saved.raised_events = order->raised_events;
            data::persist::SavedGoal goal;
            save_goal(*record, goal);
            saved.goal_kind = goal.kind;
            visit(walk, &saved, &goal);
        });
    }
}

Match::SavedOrderTails Match::saved_order_tails(uint16_t index) {
    auto& unit = units_.at(index);
    return {&unit.primary, &unit.secondary};
}

sim::simulation_state::Order& Match::restore_saved_order(
    uint16_t index,
    const data::persist::SavedOrder& saved,
    const data::persist::SavedGoal& goal,
    SavedOrderTails& tails
) {
    auto& unit = units_.at(index);
    const auto unit_at = [&](uint16_t id) -> sim::simulation_state::Unit* {
        return id != 0 && id < units_.size() && live(&units_[id]) ? &units_[id] : nullptr;
    };
    auto entry = std::make_unique<RuntimeOrder>();
    entry->unit = &unit;
    auto& order = entry->order;
    order.kind = saved.kind;
    order.phase = saved.phase;
    order.wait_events = saved.wait_events;
    order.wake_tick = saved.wake_tick;
    order.seen_x = static_cast<int16_t>(saved.seen_cell & 0xffffu);
    order.seen_z = static_cast<int16_t>(saved.seen_cell >> 16);
    // A loaded order's issue tick is stamped with the tick it loads on.
    order.issue_tick = state().game.tick;
    order.preserve_flags = saved.preserve_flags;
    order.flags = saved.flags;
    order.raised_events = saved.raised_events;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        entry->extra.destination[axis] = saved.point[axis];
        entry->attack.destination[axis] = std::bit_cast<uint32_t>(saved.point[axis]);
    }
    entry->extra.anchor_x = saved.anchor[0];
    entry->extra.anchor_z = saved.anchor[1];
    entry->attack.origin = {saved.anchor[0], saved.anchor[1]};
    entry->extra.tolerance = saved.parameter_1;
    entry->attack.weapon_slot = saved.parameter_1;
    entry->construction.type_index = saved.parameter_1;
    entry->attack.retry = saved.parameter_2;
    entry->construction.remaining = saved.parameter_2;
    entry->attack.leash = saved.parameter_3;
    entry->construction.blocked_retries = saved.parameter_3;
    entry->extra.command_flags = saved.command_flags;
    // The load links the target whatever the command flags say.
    if (auto* target = unit_at(saved.target_id)) {
        entry->construction.target = target;
        link_target_observer(*entry, target);
    }
    switch (static_cast<data::persist::SavedGoalKind>(goal.kind)) {
    case data::persist::SavedGoalKind::circle:
    case data::persist::SavedGoalKind::ring:
    case data::persist::SavedGoalKind::outline: {
        auto restored = std::make_unique<sim::ground_orders::Goal>();
        restored->order = &order;
        restored->shape = static_cast<sim::ground_orders::GoalShape>(goal.kind);
        if (restored->shape == sim::ground_orders::GoalShape::circle) {
            restored->cell = {goal.circle.cell[0], goal.circle.cell[1]};
            restored->tolerance = goal.circle.tolerance;
            restored->radius_squared = goal.circle.radius_squared;
        } else if (restored->shape == sim::ground_orders::GoalShape::ring) {
            restored->cell = {goal.ring.cell[0], goal.ring.cell[1]};
            restored->inner_range = goal.ring.inner_range;
            restored->tolerance = goal.ring.outer_range;
            restored->inner_radius_squared = goal.ring.inner_radius_squared;
            restored->radius_squared = goal.ring.outer_radius_squared;
        } else {
            restored->left = goal.outline.left;
            restored->right = goal.outline.right;
            restored->top = goal.outline.top;
            restored->bottom = goal.outline.bottom;
        }
        entry->extra.goal = std::move(restored);
        break;
    }
    case data::persist::SavedGoalKind::air_target: {
        auto& air_goal = entry->air_goal;
        const auto& from = goal.air_target;
        air_goal.kind = sim::air::AirGoalKind::target;
        air_goal.order_events = &order.raised_events;
        air_goal.flags = from.flags;
        air_goal.arrival_radius = from.arrival_radius;
        air_goal.altitude = from.altitude;
        air_goal.bearing = from.bearing;
        air_goal.query_point = from.query_point;
        auto* flier = unit_at(from.unit_id);
        auto* followed = unit_at(from.target_id);
        air_goal.unit = flier != nullptr ? &flier->record : nullptr;
        air_goal.target = followed != nullptr ? &followed->record : nullptr;
        air_goal.point = {from.point[0], from.point[1], from.point[2]};
        air_goal.stand_off = from.stand_off;
        break;
    }
    case data::persist::SavedGoalKind::air_seek: {
        auto& air_goal = entry->air_goal;
        const auto& from = goal.air_seek;
        air_goal.kind = sim::air::AirGoalKind::seek;
        air_goal.order_events = &order.raised_events;
        air_goal.flags = from.flags;
        auto* flier = unit_at(from.unit_id);
        air_goal.unit = flier != nullptr ? &flier->record : nullptr;
        air_goal.point = {from.point[0], from.point[1], from.point[2]};
        air_goal.step = {from.step[0], from.step[1], from.step[2]};
        air_goal.heading = from.heading;
        break;
    }
    case data::persist::SavedGoalKind::none:
        break;
    }
    auto*& tail = (order.flags & secondary_queue_flag) != 0 ? tails.secondary : tails.primary;
    order.next = nullptr;
    *tail = &order;
    tail = &order.next;
    adopt_order(std::move(entry));
    return order;
}

void Match::install_head_goal(uint16_t index) {
    auto& unit = units_.at(index);
    if (auto* record = runtime_order(unit.primary)) {
        TickHost host(*this);
        host.install_order_goal(slots_.at(index), *record);
    }
}

void TickHost::install_order_goal(sim::unit_spawn::Slot& s, Match::RuntimeOrder& record) {
    if (record.extra.goal) {
        auto* movement = match.ground_runtime(s.unit_index);
        if (movement == nullptr)
            return;
        movement->project_slot();
        auto flags = weapon_flags(s);
        sim::ground_orders::install_goal(
            view(s, *movement, flags), record.extra.goal.get(), match.simulation_.tick, *this
        );
        write_flags(s, flags);
    } else if (record.air_goal.kind != sim::air::AirGoalKind::none) {
        sim::air::air_driver_set_goal(&match.air_drivers_.at(s.unit_index), &record.air_goal);
    }
}

} // namespace oa::sim::match_runtime
