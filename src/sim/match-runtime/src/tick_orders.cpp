// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "tick_internal.hpp"

#include "oa/sim/match_runtime/construction_orders.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <iterator>
#include <string_view>

namespace oa::sim::match_runtime {

namespace {
// The order kind's descriptor word, or nothing for a kind outside the mission
// table.
std::optional<uint32_t> mission_descriptor(uint8_t kind) noexcept {
    if (kind >= mission_descriptor_table.size())
        return std::nullopt;
    return mission_descriptor_table[kind];
}

// The order insert frees the orders at the head of the primary queue that carry
// command flag 0x40 before inserting a primary order. A head order the match
// does not own empties the queue; an order removal that runs past its budget
// stops the freeing, and its fault is returned.
template <typename Extra>
sim::simulation_state::StepFault drop_head_overlays(
    sim::simulation_state::Unit& unit, Extra&& extra, sim::simulation_state::Host& host
) {
    std::size_t idle_steps = overlay_order_budget;
    while (unit.primary && idle_steps-- > 0) {
        auto* head = unit.primary;
        const auto* head_extra = extra(head);
        // A head order the match does not own ends the queue there.
        if (!head_extra) {
            unit.primary = nullptr;
            break;
        }
        if ((head_extra->command_flags & command_overlay) == 0)
            break;
        if (const auto fault = sim::simulation_state::remove_order(unit, *head, host);
            fault != sim::simulation_state::StepFault::none)
            return fault;
        if (unit.primary == head)
            break;
    }
    return sim::simulation_state::StepFault::none;
}

// Preserve flags (the descriptor's low byte) the order-queue insert tests or
// sets.
constexpr uint8_t descriptor_inserted = 0x01;    // ? set on every queued order
constexpr uint8_t descriptor_queue_head = 0x20;  // goes to the head of the primary queue
constexpr uint8_t descriptor_keeps_queue = 0x40; // an unqueued order leaves the queue
// Order flags bits: the order lives on the secondary queue; the order
// overlays saw its target unit and kept the cell in seen_x and seen_z.
constexpr uint8_t flags_secondary = 0x04;
constexpr uint8_t order_target_seen = 0x20;
// A queued order cancels one already queued within 16 pixels (16.16).
constexpr uint32_t cancel_reach = 0x100000;
// Whole pixels a snapped click may lie from a queued order's point.
constexpr uint32_t snapped_cancel_reach = 8;

/// Tells whether the command resolver picks the VTOL construction, repair
/// and reclaim missions for this unit.
///
/// @param world Canonical world.
/// @param index Unit slot.
/// @return True when the unit's type can fly.
bool aircraft_builder(oa::World& world, uint16_t index) {
    const auto* def = oa::world_unit_def_of(&world, oa::world_unit_at(&world, index));
    return def && (def->flags & OA_UNIT_DEF_FLAG_CAN_FLY) != 0;
}

/// Tells whether a build list's entry is a round for its builder's weapon to
/// stockpile: the type's unit name holds MAKENUKE or MAKEANTI.
///
/// @param world Canonical world.
/// @param type Type index of the entry.
/// @return True for a round; false for any other type and one outside the table.
bool stockpile_round(const oa::World& world, uint16_t type) noexcept {
    if (type >= world.unit_def_count)
        return false;
    const auto& name = world.unit_defs[type].unit_name;
    const std::string_view text(name, std::find(std::begin(name), std::end(name), '\0'));
    return text.find("MAKENUKE") != std::string_view::npos ||
           text.find("MAKEANTI") != std::string_view::npos;
}
} // namespace

sim::ground_orders::OrderState* Match::owned_extra(sim::simulation_state::Order* order) noexcept {
    auto* record = runtime_order(order);
    return record ? &record->extra : nullptr;
}

sim::simulation_state::Order& Match::refused_order() noexcept {
    spare_order_ = {};
    return spare_order_.order;
}

sim::simulation_state::Order& Match::insert_ground_order(
    uint16_t index,
    uint8_t kind,
    std::optional<sim::ground_orders::Point> destination,
    int32_t tolerance,
    uint16_t target
) {
    auto& u = units_.at(index);
    if (!u.record.type_index)
        return refused_order();
    const auto descriptor = mission_descriptor(kind);
    if (!descriptor) {
        fault_.note("order kind outside mission table");
        return refused_order();
    }
    auto entry = std::make_unique<RuntimeOrder>();
    entry->unit = &u;
    entry->order.kind = kind;
    entry->order.wake_tick = 0xffffffffu;
    entry->order.issue_tick = state().game.tick;
    const auto flags = *descriptor;
    entry->order.preserve_flags = static_cast<uint8_t>(flags);
    entry->extra.command_flags = static_cast<uint8_t>(flags >> 8);
    entry->order.flags = static_cast<uint8_t>(flags >> 16);
    if (destination)
        entry->extra.destination = *destination;
    else
        entry->extra.command_flags &= static_cast<uint8_t>(~command_has_point);
    entry->extra.tolerance = tolerance;
    // As in order creation, the target is kept only when the descriptor takes
    // one.
    if (target == 0)
        entry->extra.command_flags &= static_cast<uint8_t>(~command_has_target);
    else {
        auto& aimed = units_.at(target);
        entry->construction.target = &aimed;
        if (entry->extra.command_flags & command_has_target)
            link_target_observer(*entry, &aimed);
    }
    auto& head = (entry->order.flags & 4) ? u.secondary : u.primary;
    entry->order.next = head;
    if (const auto* old = runtime_order(head))
        entry->extra.command_flags |= old->extra.command_flags & command_overlay;
    auto* result = &adopt_order(std::move(entry)).order;
    head = result;
    return *result;
}

sim::simulation_state::Order&
Match::issue_ground_move(uint16_t index, const sim::ground_orders::Point& destination, bool queue) {
    auto& unit = units_.at(index);
    if (!ground_runtime(index) && takes_move_order(index))
        return issue_queued_command(
            index, qmove_kind, destination, queue, mission_descriptor_table[qmove_kind]
        );
    if (!unit.record.type_index || !ground_runtime(index))
        return refused_order();
    auto entry = std::make_unique<RuntimeOrder>();
    entry->unit = &unit;
    // The command resolver gives aircraft VTOL_Move; its ground-goal branch
    // waits for arrival as Move_Ground does.
    entry->order.kind = (unit.type && (unit.type->flags & OA_UNIT_DEF_FLAG_CAN_FLY))
                            ? sim::ground_orders::vtol_move_kind
                            : sim::ground_orders::move_ground_kind;
    entry->order.wake_tick = 0xffffffffu;
    entry->order.issue_tick = state().game.tick;
    // Move_Ground descriptor word 0x402; destination is present.
    entry->order.preserve_flags = 2;
    entry->extra.command_flags = 4;
    entry->extra.destination = destination;
    auto* record = &adopt_order(std::move(entry));
    TickHost host(*this);
    const auto extra = [&](sim::simulation_state::Order* order) { return owned_extra(order); };
    if (!queue)
        note_step(sim::simulation_state::clear_orders(unit, false, host));
    note_step(drop_head_overlays(unit, extra, host));
    record->order.preserve_flags |= 1;
    if (!queue)
        record->extra.command_flags |= command_unqueued;
    insert_after_queue_tail(unit, record->order, extra);
    return record->order;
}

bool Match::takes_move_order(uint16_t index) const {
    const auto& unit = units_.at(index);
    if (!unit.record.type_index)
        return false;
    return ground_runtime(index) ||
           (match_unit_def(*this, unit.record).abilities & OA_UNIT_DEF_ABILITY_CAN_MOVE) != 0;
}

sim::simulation_state::Order& Match::issue_queued_command(
    uint16_t index,
    uint8_t kind,
    const sim::ground_orders::Point& destination,
    bool queue,
    uint32_t flags
) {
    auto& unit = units_.at(index);
    if (!unit.record.type_index)
        return refused_order();
    auto entry = std::make_unique<RuntimeOrder>();
    entry->unit = &unit;
    entry->order.kind = kind;
    entry->order.wake_tick = 0xffffffffu;
    entry->order.issue_tick = state().game.tick;
    entry->order.preserve_flags = static_cast<uint8_t>(flags);
    entry->extra.command_flags = static_cast<uint8_t>(flags >> 8);
    entry->order.flags = static_cast<uint8_t>(flags >> 16);
    entry->extra.destination = destination;
    auto* record = &adopt_order(std::move(entry));
    TickHost host(*this);
    const auto extra = [&](sim::simulation_state::Order* order) { return owned_extra(order); };
    if (!queue)
        note_step(sim::simulation_state::clear_orders(unit, false, host));
    note_step(drop_head_overlays(unit, extra, host));
    record->order.preserve_flags |= 1;
    if (!queue)
        record->extra.command_flags |= command_unqueued;
    insert_after_queue_tail(unit, record->order, extra);
    return record->order;
}

sim::simulation_state::Order& Match::issue_cloak(uint16_t index, bool on) {
    return issue_state_order(index, on ? cloak_on_kind : cloak_off_kind, 0);
}

sim::simulation_state::Order&
Match::issue_state_order(uint16_t index, uint8_t kind, int32_t value) {
    auto& unit = units_.at(index);
    if (!unit.record.type_index)
        return refused_order();
    const auto descriptor = mission_descriptor(kind);
    if (!descriptor) {
        fault_.note("order kind outside mission table");
        return refused_order();
    }
    auto entry = std::make_unique<RuntimeOrder>();
    entry->unit = &unit;
    entry->order.kind = kind;
    entry->order.wake_tick = 0xffffffffu;
    entry->order.issue_tick = state().game.tick;
    entry->extra.tolerance = value;
    entry->attack.weapon_slot = value;
    const auto flags = *descriptor;
    entry->order.preserve_flags = static_cast<uint8_t>(flags);
    entry->extra.command_flags = static_cast<uint8_t>(flags >> 8);
    entry->order.flags = static_cast<uint8_t>(flags >> 16);
    auto* record = &adopt_order(std::move(entry));
    TickHost host(*this);
    const auto extra = [&](sim::simulation_state::Order* order) { return owned_extra(order); };
    // The descriptor's preserve flags bit 0x40 keeps the queue, and bit 0x20
    // puts the order at the head of the primary queue.
    note_step(drop_head_overlays(unit, extra, host));
    record->order.preserve_flags |= 1;
    record->extra.command_flags |= command_unqueued;
    record->order.next = unit.primary;
    unit.primary = &record->order;
    return record->order;
}

void Match::visit_primary_queue(
    uint16_t index, const std::function<void(const QueuedCommandView&)>& fn
) const {
    if (!fn || index >= units_.size())
        return;
    const auto& unit = units_[index];
    for_each_primary_uncycled(unit.primary, [&](sim::simulation_state::Order* order) {
        const auto* record = runtime_order(order);
        if (!record || (record->extra.command_flags & command_overlay) != 0)
            return;
        QueuedCommandView view;
        view.kind = order->kind;
        view.destination = record->extra.destination;
        view.build_type = record->construction.type_index;
        const auto* target =
            record->construction.target ? record->construction.target : record->attack.target;
        if (target) {
            const std::array<uint32_t, 3> position = target->position;
            view.destination = {
                std::bit_cast<sim::ground_orders::Fixed>(position[0]),
                std::bit_cast<sim::ground_orders::Fixed>(position[1]),
                std::bit_cast<sim::ground_orders::Fixed>(position[2])
            };
        } else if (
            view.destination[0] == 0 && view.destination[2] == 0 &&
            (record->attack.destination[0] != 0 || record->attack.destination[2] != 0)
        ) {
            view.destination = {
                std::bit_cast<sim::ground_orders::Fixed>(record->attack.destination[0]),
                std::bit_cast<sim::ground_orders::Fixed>(record->attack.destination[1]),
                std::bit_cast<sim::ground_orders::Fixed>(record->attack.destination[2])
            };
        }
        fn(view);
    });
}

sim::simulation_state::Order& Match::issue_order(
    uint16_t index,
    uint8_t kind,
    bool queue,
    uint16_t target,
    const sim::ground_orders::Point* point,
    int32_t parameter_1,
    int32_t parameter_2
) {
    auto& unit = units_.at(index);
    if (!unit.record.type_index)
        return refused_order();
    const auto found_descriptor = mission_descriptor(kind);
    if (!found_descriptor) {
        fault_.note("order kind outside mission table");
        return refused_order();
    }
    sim::simulation_state::Unit* aimed = target != 0 ? &units_.at(target) : nullptr;
    // The record order creation builds: the kind's descriptor, the target (kept
    // only when the descriptor takes one), the point and the argument words,
    // each in every order family's copy of it; the third parameter is zero.
    auto entry = std::make_unique<RuntimeOrder>();
    entry->unit = &unit;
    entry->order.kind = kind;
    entry->order.wake_tick = 0xffffffffu;
    entry->order.issue_tick = state().game.tick;
    const auto descriptor = *found_descriptor;
    entry->order.preserve_flags = static_cast<uint8_t>(descriptor);
    entry->extra.command_flags = static_cast<uint8_t>(descriptor >> 8);
    entry->order.flags = static_cast<uint8_t>(descriptor >> 16);
    if (point != nullptr) {
        entry->extra.destination = *point;
        entry->attack.destination = {
            std::bit_cast<uint32_t>((*point)[0]),
            std::bit_cast<uint32_t>((*point)[1]),
            std::bit_cast<uint32_t>((*point)[2])
        };
    } else
        entry->extra.command_flags &= static_cast<uint8_t>(~command_has_point);
    entry->extra.tolerance = parameter_1;
    entry->attack.weapon_slot = parameter_1;
    entry->attack.retry = parameter_2;
    entry->construction.type_index = parameter_1;
    entry->construction.remaining = parameter_2;
    if (aimed == nullptr)
        entry->extra.command_flags &= static_cast<uint8_t>(~command_has_target);
    else if (entry->extra.command_flags & command_has_target) {
        entry->construction.target = aimed;
        link_target_observer(*entry, aimed);
    }
    auto* record = &adopt_order(std::move(entry));
    TickHost host(*this);
    const auto extra = [&](sim::simulation_state::Order* order) { return owned_extra(order); };
    if (!queue && (record->order.preserve_flags & descriptor_keeps_queue) == 0)
        note_step(sim::simulation_state::clear_orders(unit, false, host));
    if ((record->order.flags & flags_secondary) == 0)
        note_step(drop_head_overlays(unit, extra, host));
    record->order.preserve_flags |= descriptor_inserted;
    if (!queue)
        record->extra.command_flags |= command_unqueued;
    if ((record->order.preserve_flags & descriptor_queue_head) != 0 ||
        (record->order.flags & flags_secondary) != 0) {
        // Before the head of the order's own queue.
        auto& head = (record->order.flags & flags_secondary) ? unit.secondary : unit.primary;
        if (head) {
            const auto* head_extra = extra(head);
            if (!head_extra) {
                fault_.note("order insert encounters an unowned order");
                return record->order;
            }
            record->extra.command_flags |= head_extra->command_flags & command_overlay;
        }
        record->order.next = head;
        head = &record->order;
    } else {
        insert_after_queue_tail(unit, record->order, extra);
    }
    return record->order;
}

bool Match::cancel_queued_order(
    uint16_t index,
    uint8_t kind,
    uint16_t target,
    const sim::ground_orders::Point* point,
    bool snapped_point
) {
    auto& unit = units_.at(index);
    const sim::simulation_state::Unit* aimed = target != 0 ? &units_.at(target) : nullptr;
    // Both differences are taken with 32-bit wraparound, as 3.1c takes them.
    // A snapped click compares whole pixels, the order's less the click's.
    const auto near = [snapped_point](sim::ground_orders::Fixed a, sim::ground_orders::Fixed b) {
        if (snapped_point) {
            const auto click =
                static_cast<uint32_t>(static_cast<uint16_t>(std::bit_cast<uint32_t>(a) >> 16));
            const auto order =
                static_cast<uint32_t>(static_cast<uint16_t>(std::bit_cast<uint32_t>(b) >> 16));
            return order - click + snapped_cancel_reach < 2 * snapped_cancel_reach;
        }
        return std::bit_cast<uint32_t>(a) - std::bit_cast<uint32_t>(b) + cancel_reach <=
               2 * cancel_reach;
    };
    sim::simulation_state::Order* found = nullptr;
    for_each_primary_uncycled(unit.primary, [&](sim::simulation_state::Order* order) {
        if (found != nullptr || order->kind != kind)
            return;
        const auto* record = runtime_order(order);
        if (record == nullptr)
            return;
        const auto* order_target =
            record->attack.target ? record->attack.target : record->construction.target;
        if (aimed != nullptr && aimed != order_target)
            return;
        if (point != nullptr && (!near((*point)[0], record->extra.destination[0]) ||
                                 !near((*point)[2], record->extra.destination[2])))
            return;
        found = order;
    });
    if (found == nullptr)
        return false;
    TickHost host(*this);
    note_step(sim::simulation_state::remove_order(unit, *found, host));
    return true;
}

void Match::issue_or_cancel_order(
    uint16_t index,
    uint8_t kind,
    bool queue,
    uint16_t target,
    const sim::ground_orders::Point* point,
    int32_t parameter_1,
    int32_t parameter_2
) {
    if (queue && cancel_queued_order(index, kind, target, point))
        return;
    (void)issue_order(index, kind, queue, target, point, parameter_1, parameter_2);
}

std::size_t Match::queue_records(
    uint16_t index, bool secondary, OrderRecordView* out, std::size_t capacity
) const {
    if (index >= units_.size())
        return 0;
    const auto& unit = units_[index];
    std::size_t written = 0;
    auto* head = secondary ? unit.secondary : unit.primary;
    for_each_primary_uncycled(head, [&](sim::simulation_state::Order* order) {
        if (written == capacity)
            return;
        if (const auto* candidate = runtime_order(order)) {
            auto& view = out[written++];
            view.kind = order->kind;
            view.preserve_flags = order->preserve_flags;
            view.command_flags = candidate->extra.command_flags;
            view.flags = order->flags;
            const auto* aimed = candidate->attack.target ? candidate->attack.target
                                                         : candidate->construction.target;
            view.target = aimed ? aimed->record.id : uint16_t{0};
            view.point = candidate->extra.destination;
            view.parameter_1 = candidate->construction.type_index;
            view.parameter_2 = candidate->construction.remaining;
            view.seen_x = order->seen_x;
            view.seen_z = order->seen_z;
            view.issue_tick = order->issue_tick;
            view.phase = order->phase;
            view.facing = candidate->construction.facing;
            view.build_started = candidate->construction.target != nullptr;
        }
    });
    return written;
}

void Match::note_order_target_seen(uint16_t index, std::size_t position, int16_t x, int16_t z) {
    if (index >= units_.size())
        return;
    std::size_t at = 0;
    for_each_primary_uncycled(units_[index].primary, [&](sim::simulation_state::Order* order) {
        if (at++ != position)
            return;
        order->seen_x = x;
        order->seen_z = z;
        order->flags |= order_target_seen;
    });
}

sim::simulation_state::Order& Match::issue_mobile_build(
    uint16_t index,
    uint16_t type,
    const sim::ground_orders::Point& destination,
    bool queue,
    uint8_t facing
) {
    const auto kind = aircraft_builder(state(), index) ? vtol_mobile_build_kind : mobile_build_kind;
    auto& order = issue_queued_command(index, kind, destination, queue, mobile_build_flags);
    if (auto* candidate = runtime_order(&order)) {
        candidate->construction.type_index = type;
        candidate->construction.facing = static_cast<uint8_t>(facing & 3U);
    }
    return order;
}

sim::simulation_state::Order&
Match::issue_patrol(uint16_t index, const sim::ground_orders::Point& destination, bool queue) {
    if (!ground_runtime(index))
        return issue_queued_command(
            index, qpatrol_kind, destination, queue, mission_descriptor_table[qpatrol_kind]
        );
    const auto& def = match_unit_def(*this, units_.at(index).record);
    if (def.abilities & OA_UNIT_DEF_ABILITY_CAN_REPAIR)
        return issue_repair_patrol(index, destination, queue);
    const auto kind = (def.flags & OA_UNIT_DEF_FLAG_CAN_FLY) ? vtol_patrol_kind : patrol_kind;
    return issue_queued_command(index, kind, destination, queue, mission_descriptor_table[kind]);
}

sim::simulation_state::Order& Match::issue_repair_patrol(
    uint16_t index, const sim::ground_orders::Point& destination, bool queue
) {
    const auto flying =
        (match_unit_def(*this, units_.at(index).record).flags & OA_UNIT_DEF_FLAG_CAN_FLY) != 0;
    const auto kind = flying ? vtol_repair_patrol_kind : repair_patrol_kind;
    return issue_queued_command(index, kind, destination, queue, mission_descriptor_table[kind]);
}

sim::simulation_state::Order& Match::issue_feature_reclaim(
    uint16_t index, const sim::ground_orders::Point& destination, bool queue
) {
    const auto flying =
        (match_unit_def(*this, units_.at(index).record).flags & OA_UNIT_DEF_FLAG_CAN_FLY) != 0;
    const auto kind = flying ? vtol_reclaim_kind : reclaim_kind;
    return issue_queued_command(index, kind, destination, queue, mission_descriptor_table[kind]);
}

sim::simulation_state::Order&
Match::issue_build_weapon(uint16_t index, int32_t weapon_slot, int32_t count) {
    constexpr uint8_t build_weapon_kind = 13;
    auto& order = insert_ground_order(index, build_weapon_kind);
    if (auto* candidate = runtime_order(&order)) {
        candidate->attack.weapon_slot = weapon_slot;
        candidate->construction.type_index = weapon_slot;
        candidate->construction.remaining = count;
    }
    return order;
}

sim::simulation_state::Order& Match::issue_attack_special(
    uint16_t index, const sim::ground_orders::Point& destination, bool queue, uint16_t target
) {
    // Command 4: AttackSpecial. Handler morphs to Chase/Suppress with slot 2.
    auto& order = issue_queued_command(index, attack_special_kind, destination, queue, 0x680u);
    if (auto* candidate = runtime_order(&order)) {
        candidate->attack.destination = {
            std::bit_cast<uint32_t>(destination[0]),
            std::bit_cast<uint32_t>(destination[1]),
            std::bit_cast<uint32_t>(destination[2])
        };
        candidate->attack.weapon_slot = 2;
        if (target != 0)
            link_target_observer(*candidate, &units_.at(target));
    }
    return order;
}

sim::simulation_state::Order&
Match::issue_building_build(uint16_t index, uint16_t type, int32_t count, bool queue) {
    auto& unit = units_.at(index);
    sim::ground_orders::Point here{
        signed_word(unit.position[0]), signed_word(unit.position[1]), signed_word(unit.position[2])
    };
    auto& order =
        issue_queued_command(index, building_build_kind, here, queue, building_build_flags);
    if (auto* candidate = runtime_order(&order)) {
        candidate->construction.type_index = type;
        candidate->construction.remaining = count < 1 ? 1 : count;
    }
    return order;
}

int32_t Match::queued_build_count(uint16_t index, int32_t type) const {
    int32_t total = 0;
    const auto& unit = units_.at(index);
    const auto count = [&](sim::simulation_state::Order* order) {
        const auto* candidate = runtime_order(order);
        if (candidate && (candidate->extra.command_flags & command_counts_builds) != 0 &&
            candidate->construction.type_index == type)
            total += candidate->construction.remaining;
    };
    for_each_primary_uncycled(unit.primary, count);
    for_each_primary_uncycled(unit.secondary, count);
    return total;
}

void Match::change_queued_count(uint16_t index, uint8_t kind, uint16_t type, int32_t delta) {
    auto& unit = units_.at(index);
    const auto descriptor = mission_descriptor(kind);
    if (!descriptor) {
        fault_.note("order kind outside mission table");
        return;
    }
    const bool secondary = ((*descriptor >> 16) & flags_secondary) != 0;
    const auto owned = [&](sim::simulation_state::Order* order) -> RuntimeOrder* {
        return runtime_order(order);
    };
    const auto matches = [&](sim::simulation_state::Order* order, RuntimeOrder* record) {
        return record != nullptr && order->kind == kind &&
               static_cast<uint32_t>(record->construction.type_index) == type;
    };
    if (delta > 0) {
        sim::simulation_state::Order* last = nullptr;
        for_each_primary_uncycled(
            secondary ? unit.secondary : unit.primary,
            [&](sim::simulation_state::Order* order) { last = order; }
        );
        if (last != nullptr) {
            if (auto* record = owned(last); matches(last, record)) {
                record->construction.remaining += delta;
                return;
            }
        }
        (void)issue_order(index, kind, true, 0, nullptr, type, delta);
        return;
    }
    TickHost host(*this);
    for (;;) {
        sim::simulation_state::Order* latest = nullptr;
        RuntimeOrder* latest_record = nullptr;
        for_each_primary_uncycled(
            secondary ? unit.secondary : unit.primary, [&](sim::simulation_state::Order* order) {
                if (auto* record = owned(order); matches(order, record)) {
                    latest = order;
                    latest_record = record;
                }
            }
        );
        if (latest == nullptr)
            return;
        if (latest_record->construction.remaining > -delta) {
            latest_record->construction.remaining += delta;
            return;
        }
        delta += latest_record->construction.remaining;
        note_step(sim::simulation_state::remove_order(unit, *latest, host));
    }
}

void Match::queue_factory_build(uint16_t index, uint16_t type, int32_t delta) {
    constexpr uint8_t build_weapon_kind = 13;
    if (stockpile_round(state(), type)) {
        // Rounds for the first weapon slot: the order's first parameter is the slot.
        change_queued_count(index, build_weapon_kind, 0, delta);
        return;
    }
    change_queued_count(index, building_build_kind, type, delta);
}

sim::simulation_state::Order& Match::issue_help_build(uint16_t index, uint16_t target, bool queue) {
    auto& assisted = units_.at(target);
    sim::ground_orders::Point here{
        signed_word(assisted.position[0]),
        signed_word(assisted.position[1]),
        signed_word(assisted.position[2])
    };
    const auto kind = aircraft_builder(state(), index) ? vtol_help_build_kind : help_build_kind;
    auto& order = issue_queued_command(index, kind, here, queue, help_build_flags);
    if (auto* candidate = runtime_order(&order)) {
        candidate->construction.target = &assisted;
        candidate->construction.type_index = static_cast<int32_t>(assisted.record.type_index);
        link_target_observer(*candidate, &assisted);
    }
    return order;
}

sim::simulation_state::Order& Match::issue_repair(uint16_t index, uint16_t target, bool queue) {
    auto& unit = units_.at(index);
    auto& repaired = units_.at(target);
    sim::ground_orders::Point here{
        signed_word(repaired.position[0]),
        signed_word(repaired.position[1]),
        signed_word(repaired.position[2])
    };
    const auto kind = aircraft_builder(state(), index) ? vtol_repair_unit_kind : repair_unit_kind;
    auto& order = issue_queued_command(index, kind, here, queue, repair_unit_flags);
    if (auto* candidate = runtime_order(&order)) {
        candidate->construction.target = &repaired;
        candidate->construction.type_index = static_cast<int32_t>(repaired.record.type_index);
        link_target_observer(*candidate, &repaired);
    }
    (void)unit;
    return order;
}

sim::simulation_state::Order& Match::issue_reclaim(uint16_t index, uint16_t target, bool queue) {
    auto& reclaimed = units_.at(target);
    sim::ground_orders::Point here{
        signed_word(reclaimed.position[0]),
        signed_word(reclaimed.position[1]),
        signed_word(reclaimed.position[2])
    };
    const auto kind = aircraft_builder(state(), index) ? vtol_reclaim_unit_kind : reclaim_unit_kind;
    auto& order = issue_queued_command(index, kind, here, queue, reclaim_unit_flags);
    if (auto* candidate = runtime_order(&order)) {
        candidate->construction.target = &reclaimed;
        link_target_observer(*candidate, &reclaimed);
    }
    return order;
}

sim::simulation_state::Order& Match::issue_capture(uint16_t index, uint16_t target, bool queue) {
    auto& captured = units_.at(target);
    sim::ground_orders::Point here{
        signed_word(captured.position[0]),
        signed_word(captured.position[1]),
        signed_word(captured.position[2])
    };
    auto& order = issue_queued_command(index, capture_kind, here, queue, capture_flags);
    if (auto* candidate = runtime_order(&order)) {
        candidate->construction.target = &captured;
        link_target_observer(*candidate, &captured);
    }
    return order;
}

sim::simulation_state::Order& Match::issue_load(uint16_t index, uint16_t cargo, bool queue) {
    auto& carried = units_.at(cargo);
    sim::ground_orders::Point here{
        signed_word(carried.position[0]),
        signed_word(carried.position[1]),
        signed_word(carried.position[2])
    };
    // Command 6: VTOL_Pickup for aircraft (same descriptor).
    const auto kind =
        (match_unit_def(*this, units_.at(index).record).flags & OA_UNIT_DEF_FLAG_CAN_FLY)
            ? vtol_pickup_kind
            : ground_pickup_kind;
    auto& order = issue_queued_command(index, kind, here, queue, ground_pickup_flags);
    if (auto* candidate = runtime_order(&order)) {
        candidate->construction.target = &carried;
        link_target_observer(*candidate, &carried);
    }
    return order;
}

sim::simulation_state::Order&
Match::issue_unload(uint16_t index, const sim::ground_orders::Point& destination, bool queue) {
    // Command 5 without an air base target: VTOL_Unload for aircraft.
    const auto kind =
        (match_unit_def(*this, units_.at(index).record).flags & OA_UNIT_DEF_FLAG_CAN_FLY)
            ? vtol_unload_kind
            : ground_unload_kind;
    auto& order = issue_queued_command(index, kind, destination, queue, ground_unload_flags);
    return order;
}

sim::simulation_state::Order& Match::issue_guard(uint16_t index, uint16_t target, bool queue) {
    auto& guarded = units_.at(target);
    sim::ground_orders::Point here{
        signed_word(guarded.position[0]),
        signed_word(guarded.position[1]),
        signed_word(guarded.position[2])
    };
    // Command 7: VTOL_Follow for aircraft (same descriptor).
    const auto kind =
        (match_unit_def(*this, units_.at(index).record).flags & OA_UNIT_DEF_FLAG_CAN_FLY)
            ? vtol_follow_kind
            : follow_ground_kind;
    auto& order = issue_queued_command(index, kind, here, queue, follow_ground_flags);
    if (auto* candidate = runtime_order(&order)) {
        candidate->construction.target = &guarded;
        link_target_observer(*candidate, &guarded);
    }
    return order;
}

bool Match::commit_attack_orders(
    const sim::combat_state::AttackSource& source,
    std::span<const sim::combat_state::AttackOrderRequest> requests
) {
    auto& unit = units_.at(source.identity);
    for (const auto& request : requests) {
        if (request.kind != attack_chase_kind && request.kind != attack_no_move_kind &&
            request.kind != attack_special_kind && request.kind != 26 &&
            request.kind != sim::ground_orders::vtol_move_kind &&
            request.kind != air_to_ground_kind && request.kind != air_to_air_kind &&
            request.kind != air_strike_kind && request.kind != air_to_ground_hover_kind &&
            request.kind != suppress_kind && request.kind != vtol_seek_attack_kind) {
            fault_.note("attack request descriptor");
            return false;
        }
        auto entry = std::make_unique<RuntimeOrder>();
        entry->unit = &unit;
        entry->order.kind = request.kind;
        entry->order.wake_tick = 0xffffffffu;
        entry->order.issue_tick = state().game.tick;
        const uint32_t flags =
            (request.kind == 26 || request.kind == sim::ground_orders::vtol_move_kind) ? 0x402u
                                                                                       : 0x280u;
        entry->order.preserve_flags = static_cast<uint8_t>(flags);
        entry->extra.command_flags = static_cast<uint8_t>(flags >> 8);
        entry->order.flags = static_cast<uint8_t>(flags >> 16);
        if (request.has_position) {
            for (std::size_t i = 0; i < 3; ++i)
                entry->extra.destination[i] = std::bit_cast<int32_t>(request.position[i]);
            entry->attack.destination = request.position;
        } else
            entry->extra.command_flags &= static_cast<uint8_t>(~command_has_point);
        if (request.target == 0)
            entry->extra.command_flags &= static_cast<uint8_t>(~command_has_target);
        else if (entry->extra.command_flags & command_has_target)
            link_target_observer(*entry, &units_.at(request.target));
        // The leash and the anchor are one value each; every view of the order holds them.
        entry->attack.leash = request.leash_length;
        entry->construction.blocked_retries = request.leash_length;
        entry->attack.origin = {request.source_x, request.source_z};
        entry->extra.anchor_x = request.source_x;
        entry->extra.anchor_z = request.source_z;
        auto* record = &adopt_order(std::move(entry));
        auto& head = (record->order.flags & 4) ? unit.secondary : unit.primary;
        record->order.next = head;
        if (const auto* prior = runtime_order(head))
            record->extra.command_flags |= prior->extra.command_flags & command_overlay;
        head = &record->order;
    }
    return true;
}

sim::simulation_state::Order& Match::issue_stop(uint16_t index) {
    const auto& unit = units_.at(index);
    const sim::ground_orders::Point here{
        signed_word(unit.position[0]), signed_word(unit.position[1]), signed_word(unit.position[2])
    };
    return issue_queued_command(index, stop_kind, here, false, mission_descriptor_table[stop_kind]);
}

void Match::stop_orders(uint16_t index) {
    TickHost host(*this);
    note_step(sim::simulation_state::clear_orders(units_.at(index), true, host));
}

bool TickHost::relocate_queued_build(
    uint16_t index, std::size_t position, const sim::ground_orders::Point& destination
) {
    if (index >= match.units_.size() || index >= match.slots_.size())
        return false;
    auto& unit = match.units_[index];
    sim::simulation_state::Order* order = nullptr;
    std::size_t at = 0;
    for_each_primary_uncycled(unit.primary, [&](sim::simulation_state::Order* queued) {
        if (at == position)
            order = queued;
        ++at;
    });
    if (order == nullptr)
        return false;
    const bool mobile = order->kind == mobile_build_kind;
    const bool aircraft = order->kind == vtol_mobile_build_kind;
    if (!mobile && !aircraft)
        return false;
    // MobileBuild spawns its nanoframe at the end of phase 1, and
    // VTOL_MobileBuild at the end of phase 2. Later phases are the building.
    if (mobile && order->phase > 1)
        return false;
    if (aircraft && order->phase > 2)
        return false;
    auto* record = match.runtime_order(order);
    if (record == nullptr || record->construction.target != nullptr)
        return false;
    const auto type = static_cast<uint16_t>(record->construction.type_index);
    auto& world = match.state();
    if (type == 0 || type >= world.unit_def_count)
        return false;
    const auto facing = match.build_facing(type, record->construction.facing);
    const auto& built = world.unit_defs[type];
    const bool turned = (facing & 1U) != 0;
    const auto footprint_x = turned ? built.footprint_z : built.footprint_x;
    const auto footprint_z = turned ? built.footprint_x : built.footprint_z;
    const auto cell_x = footprint_origin_cell(destination[0], footprint_x);
    const auto cell_z = footprint_origin_cell(destination[2], footprint_z);
    auto site = destination;
    snap_build_position(site, footprint_x, footprint_z);
    const auto height = match.footprint_height(type, cell_x, cell_z, facing);
    site[1] = static_cast<int32_t>(static_cast<uint32_t>(height) << 16);
    record->extra.destination = site;
    record->construction.blocked_retries = 0;

    // The approach already underway would finish at the old site and then
    // build at the new one. Start it again so the next step aims at the site
    // just stored: MobileBuild's first phase, VTOL_MobileBuild's fly-to phase.
    const bool head = unit.primary == order;
    auto& slot = match.slots_[index];
    if (head && mobile && order->phase == 1) {
        order->phase = 0;
        order->wait_events = 0;
        if (auto* movement = match.ground_runtime(index)) {
            movement->project_slot();
            auto flags = weapon_flags(slot);
            if (record->extra.goal) {
                sim::ground_orders::install_goal(
                    view(slot, *movement, flags), nullptr, match.simulation_.tick, *this
                );
                record->extra.goal.reset();
            }
            write_flags(slot, flags);
        }
    } else if (head && aircraft && order->phase == 2) {
        order->phase = 1;
        order->wait_events = 0;
        if (index < match.air_drivers_.size() && match.air_drivers_[index].unit != nullptr)
            install_air_goal(slot, *order, nullptr);
    }
    // A path that already failed for the old site must not fail the new one.
    order->raised_events &= ~sim::ground_orders::goal_event_mask;
    return true;
}

bool Match::relocate_queued_build(
    uint16_t index, std::size_t position, const sim::ground_orders::Point& destination
) {
    return TickHost(*this).relocate_queued_build(index, position, destination);
}

} // namespace oa::sim::match_runtime
