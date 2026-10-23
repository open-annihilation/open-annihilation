// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/simulation_state.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace oa::sim::simulation_state {
namespace {
constexpr uint32_t timer_event = 1, weapon_event = 0x10000;
constexpr uint8_t secondary_flag = 4, detached_flag = 1, retry_flag = 0x80;
constexpr uint32_t height_dirty = OA_UNIT_FLAG_POSITION_DIRTY,
                   can_hover_type = OA_UNIT_DEF_FLAG_CAN_HOVER;
constexpr uint8_t live_multiplayer_game = 1, periodic_enabled = 2;

// Takes one step from the budget; false when none is left.
[[nodiscard]] bool spend(std::size_t& budget) noexcept {
    if (budget == 0)
        return false;
    --budget;
    return true;
}

void wait(oa::World& w, Order& o, Host& h, uint32_t range) {
    const auto random = h.random_bounded(range);
    o.wait_events |= timer_event;
    o.wake_tick = w.game.tick + random + 30u;
}

const oa::UnitDef* type(oa::World& w, const oa::Unit& u) noexcept {
    return oa::world_unit_def_of(&w, &u);
}

// UnitDef.water_line is read as an unsigned byte.
uint32_t waterline(const oa::UnitDef& t) noexcept {
    return static_cast<uint8_t>(t.water_line);
}

uint32_t position_word(oa::oa_fixed value) noexcept {
    return static_cast<uint32_t>(value);
}
} // namespace

uint8_t lowest_unused_player_mark(const oa::World& w) noexcept {
    for (int32_t mark = 1; mark <= 10; ++mark) {
        bool found = false;
        for (const auto& player : w.game.players)
            if (player_active(player) && static_cast<int32_t>(player.machine_group) == mark)
                found = true;
        if (!found)
            return static_cast<uint8_t>(mark);
    }
    return 0;
}

oa::Unit* nearest_candidate_unit(
    oa::World& w,
    std::span<const uint8_t> relation,
    int32_t x,
    int32_t z,
    const CandidateFilter& filter
) noexcept {
    // The occupancy bit set in states 2 and 3, and the move-rate bit set in tiers 2 and 3.
    constexpr uint32_t occupancy_upper_states = 0x2u;
    constexpr uint32_t move_rate_upper_tiers = 0x8u;
    constexpr uint32_t widened_skip =
        occupancy_upper_states | move_rate_upper_tiers | OA_UNIT_FLAG_NOT_SELECTABLE;
    // Unit.last_occupy_code of a unit fully under water.
    constexpr uint32_t submerged_occupy_code = 3;
    oa::Unit* best = nullptr;
    auto best_metric = static_cast<int32_t>(0x7fffffff);
    for (uint8_t index = 0; index < OA_PLAYER_COUNT; ++index) {
        const auto& player = w.game.players[index];
        if (!player_slot_active(index, player) || relation[player.index] != 0)
            continue;
        uint32_t count = 0;
        auto* units = oa::world_player_units(&w, &player, &count);
        for (uint32_t i = 0; i < count; ++i) {
            auto& unit = units[i];
            const auto flags = unit.flags;
            if ((flags & OA_UNIT_FLAG_LIVE) == 0 || (unit.state_flags & OA_UNIT_STATE_CLOAKED) != 0)
                continue;
            if (filter.widened_flags) {
                if ((flags & widened_skip) != 0)
                    continue;
            } else if (
                (flags & OA_UNIT_FLAG_OCCUPANCY_MASK) == 2 ||
                (flags & OA_UNIT_FLAG_NOT_SELECTABLE) != 0
            ) {
                continue;
            }
            if (filter.skip_submerged) {
                uint32_t occupy_code = 0;
                std::memcpy(&occupy_code, unit.last_occupy_code, sizeof occupy_code);
                if (occupy_code == submerged_occupy_code)
                    continue;
            }
            const auto dx =
                static_cast<int32_t>(static_cast<uint32_t>(x) - position_word(unit.position.x));
            const auto dz =
                static_cast<int32_t>(static_cast<uint32_t>(z) - position_word(unit.position.z));
            const auto x_high = static_cast<uint32_t>(
                static_cast<uint64_t>(static_cast<int64_t>(dx) * static_cast<int64_t>(dx)) >> 32
            );
            const auto z_high = static_cast<uint32_t>(
                static_cast<uint64_t>(static_cast<int64_t>(dz) * static_cast<int64_t>(dz)) >> 32
            );
            const auto metric = static_cast<int32_t>(x_high + z_high);
            if (metric < best_metric) {
                best = &unit;
                best_metric = metric;
            }
        }
    }
    return best;
}

bool locally_simulated(const oa::World& w, const oa::Unit& u) noexcept {
    const auto* owner = oa::world_unit_owner(&w, &u);
    return owner && owner->in_use && (owner->status == 1 || owner->status == 2);
}

bool unit_selectable(const oa::World& w, const oa::Unit& u) {
    const auto unfinished = u.build_remaining;
    // The float compare accepts unordered as well as equal.
    if (!(u.flags & OA_UNIT_FLAG_SELECTABLE) || (unfinished != 0 && !std::isnan(unfinished)) ||
        u.capture_cooldown)
        return false;
    const auto parent = oa::oa_unit_slot_from_ref(u.attach_parent);
    if (parent >= w.unit_slot_count)
        return false;
    return !parent || (w.units[parent].flags & OA_UNIT_FLAG_AIR_BASE) != 0;
}

StepFault remove_order(OrderQueue& q, oa::Unit& u, Order& o, Host& h) {
    Order* original = q.primary;
    Order** link = (o.flags & secondary_flag) ? &q.secondary : &q.primary;
    std::size_t budget = default_order_budget;
    while (*link && *link != &o) {
        if (!spend(budget))
            return StepFault::order_budget_spent;
        link = &(*link)->next;
    }
    if (!*link)
        return StepFault::none;
    *link = o.next;
    if (&o != original)
        o.flags |= detached_flag;
    h.destroy_order(u, o);
    return StepFault::none;
}

StepFault rotate_primary(OrderQueue& q, Order& o) {
    Order** link = &q.primary;
    std::size_t budget = default_order_budget;
    while (*link != &o) {
        if (!spend(budget))
            return StepFault::order_budget_spent;
        if (!*link)
            return StepFault::order_not_queued;
        link = &(*link)->next;
    }
    // The tail is found before the order is unlinked, so a walk that runs out
    // of budget leaves the queue as it was.
    Order** tail = &o.next;
    while (*tail) {
        if (!spend(budget))
            return StepFault::order_budget_spent;
        tail = &(*tail)->next;
    }
    if (tail == &o.next)
        return StepFault::none;
    *link = o.next;
    *tail = &o;
    o.next = nullptr;
    return StepFault::none;
}

StepFault clear_orders(OrderQueue& q, oa::Unit& u, bool all, Host& h) {
    Order* original = q.primary;
    Order** link = &q.primary;
    std::size_t budget = default_order_budget;
    while (*link) {
        if (!spend(budget))
            return StepFault::order_budget_spent;
        Order& o = **link;
        if (!all && (o.preserve_flags & 4))
            link = &o.next;
        else {
            *link = o.next;
            if (&o != original)
                o.flags |= detached_flag;
            h.destroy_order(u, o);
        }
    }
    if (all)
        while (q.secondary) {
            if (!spend(budget))
                return StepFault::order_budget_spent;
            if (const auto fault = remove_order(q, u, *q.secondary, h); fault != StepFault::none)
                return fault;
        }
    return StepFault::none;
}

namespace {
/// Reports an order its handler ended, when the host asks to hear it.
void report_ended(Host& host, const oa::Unit& unit, const Order& order, bool done) {
    if (host.order_ends.ended != nullptr)
        host.order_ends.ended(host.order_ends.context, unit, order, done);
}
} // namespace

StepFault primary_orders(oa::World& w, OrderQueue& q, oa::Unit& u, Host& h, std::size_t budget) {
    for (;;) {
        if (!spend(budget))
            return StepFault::order_budget_spent;
        Order* o = q.primary;
        if (!o) {
            if (locally_simulated(w, u)) {
                const auto* def = type(w, u);
                if (!def)
                    return StepFault::untyped_unit;
                if (def->default_mission_type != 0)
                    h.queue_default_mission(w, u);
            }
            return StepFault::none;
        }
        if (o->wake_tick <= w.game.tick) {
            o->wake_tick = 0xffffffffu;
            o->raised_events |= timer_event;
        }
        const auto events = (static_cast<uint32_t>(u.events) | o->raised_events) & o->wait_events;
        if (o->wait_events && !events)
            return StepFault::none;
        u.events = static_cast<uint16_t>(u.events & ~events);
        o->wait_events = 0;
        o->raised_events &= ~events;
        if (events & weapon_event)
            for (uint32_t slot = 0; slot < 3; ++slot)
                h.clear_weapon_target(u, slot);
        const auto result = h.dispatch_mission(w, u, *o, events);
        switch (result) {
        case 0:
            o->phase = 0;
            break;
        case 1:
            ++o->phase;
            break;
        case 2:
        case 4:
            break;
        case 3:
            wait(w, *o, h, 15);
            break;
        case 5:
        case 8:
            report_ended(h, u, *o, result == 5);
            if (const auto fault = remove_order(q, u, *o, h); fault != StepFault::none)
                return fault;
            break;
        case 6:
            if (const auto fault = rotate_primary(q, *o); fault != StepFault::none)
                return fault;
            break;
        case 9:
            o->flags |= retry_flag;
            if (o->next) {
                report_ended(h, u, *o, false);
                if (const auto fault = remove_order(q, u, *o, h); fault != StepFault::none)
                    return fault;
            } else {
                o->phase = 0;
                wait(w, *o, h, 30);
            }
            break;
        default:
            report_ended(h, u, *o, false);
            return clear_orders(q, u, true, h);
        }
    }
}

StepFault secondary_orders(oa::World& w, OrderQueue& q, oa::Unit& u, Host& h, std::size_t budget) {
    for (;;) {
        Order* o = q.secondary;
        while (o && o->wait_events && o->wake_tick > w.game.tick) {
            if (!spend(budget))
                return StepFault::order_budget_spent;
            o = o->next;
        }
        if (!o)
            return StepFault::none;
        if (!spend(budget))
            return StepFault::order_budget_spent;
        o->wait_events = 0;
        const auto result = h.dispatch_mission(w, u, *o, 0);
        switch (result) {
        case 0:
            o->phase = 0;
            break;
        case 1:
            ++o->phase;
            break;
        case 2:
        case 4:
            break;
        case 3:
            wait(w, *o, h, 15);
            break;
        case 6:
        case 7:
            return remove_order(q, u, *o, h);
        default:
            if (const auto fault = remove_order(q, u, *o, h); fault != StepFault::none)
                return fault;
            break;
        }
    }
}

StepFault update_height(oa::World& w, oa::Unit& u, Host& h) {
    const auto flags = u.flags;
    const auto* def = type(w, u);
    if (!def)
        return StepFault::untyped_unit;
    const auto& t = *def;
    if (!(flags & height_dirty) && !(t.flags & can_hover_type))
        return StepFault::none;
    u.flags &= ~height_dirty;
    if (!u.movement || (flags & OA_UNIT_FLAG_OCCUPANCY_MASK) != 1)
        return StepFault::none;
    if (t.flags & OA_UNIT_DEF_FLAG_UPRIGHT) {
        int32_t height;
        if (!(t.flags & can_hover_type))
            height = h.terrain_height_under(u);
        else {
            height = static_cast<int32_t>(w.game.sea_level) - static_cast<int32_t>(waterline(t));
            if (height < h.terrain_height_under(u))
                height = h.terrain_height_under(u);
        }
        u.position.y = static_cast<oa::oa_fixed>(static_cast<uint32_t>(height) << 16);
    } else if (t.flags & OA_UNIT_DEF_FLAG_FLOATER) {
        u.position.y = static_cast<oa::oa_fixed>((waterline(t) * 0xffffu + w.game.sea_level) << 16);
    } else
        h.settle_on_ground(u);
    return StepFault::none;
}

bool self_heal_due(
    const data::match_rules::RepairHealtimeSelfHeal& rule,
    int16_t heal_time,
    int16_t health,
    uint32_t maximum_health,
    float build_remaining,
    uint32_t tick
) noexcept {
    constexpr uint32_t base_cadence_mask = 7;
    constexpr uint32_t heal_time_mask_bits = 0xff;
    if (heal_time == 0 || static_cast<uint32_t>(static_cast<int32_t>(health)) >= maximum_health)
        return false;
    if (rule.skip_under_construction && std::bit_cast<uint32_t>(build_remaining) != 0)
        return false;
    if (rule.cadence == data::match_rules::RepairHealtimeSelfHealCadence::healtime_mask)
        return (tick & (static_cast<uint32_t>(static_cast<uint16_t>(heal_time)) &
                        heal_time_mask_bits)) == 0;
    return (tick & base_cadence_mask) == 0;
}

float self_heal_rate(
    const data::match_rules::RepairHealtimeSelfHeal& rule, int16_t heal_time
) noexcept {
    constexpr int64_t work_per_heal_time = 8;
    constexpr int64_t ticks_per_worker_second = 30;
    const int64_t multiplier = rule.work_multiplier;
    if (rule.cadence == data::match_rules::RepairHealtimeSelfHealCadence::healtime_mask) {
        const int64_t work = static_cast<int64_t>(heal_time) * work_per_heal_time * multiplier;
        return static_cast<float>(static_cast<int32_t>(work / ticks_per_worker_second));
    }
    const auto work = static_cast<uint64_t>(static_cast<uint16_t>(heal_time)) *
                      static_cast<uint64_t>(work_per_heal_time) * static_cast<uint64_t>(multiplier);
    return static_cast<float>(
        static_cast<uint32_t>(work / static_cast<uint64_t>(ticks_per_worker_second))
    );
}

StepFault update_unit(oa::World& w, OrderQueue& q, oa::Unit& u, Host& h) {
    if (u.script)
        h.tick_script(u, 1);
    if (u.damage_countdown)
        --u.damage_countdown;
    if (u.capture_cooldown)
        --u.capture_cooldown;
    if ((u.flags & OA_UNIT_FLAG_SELECTED) && !unit_selectable(w, u))
        u.flags &= ~OA_UNIT_FLAG_SELECTED;
    const auto* def = type(w, u);
    if (w.game.tick % 30 == 0) {
        if (!def)
            return StepFault::untyped_unit;
        const auto maximum = def->max_damage;
        // The division treats the sign-extended health*100 as an unsigned word.
        // A type with no maximum health takes a quotient of 0.
        const auto quotient =
            maximum != 0 ? static_cast<uint32_t>(static_cast<int32_t>(u.health) * 100) / maximum
                         : 0u;
        const auto percent = std::clamp(std::bit_cast<int32_t>(quotient), 0, 100);
        u.previous_health_percent = u.health_percent;
        u.health_percent = static_cast<uint8_t>(percent);
    }
    if (locally_simulated(w, u)) {
        if (!def)
            return StepFault::untyped_unit;
        if (w.environment_enabled && w.environment_damage && w.game.tick % 30 == 0 &&
            std::bit_cast<int16_t>(static_cast<uint16_t>(position_word(u.position.y) >> 16)) <=
                w.game.sea_level &&
            !(def->flags & can_hover_type))
            h.apply_scaled_damage(u, w.environment_damage, 11);
        // The regeneration test reads the type again, after the damage.
        def = type(w, u);
        if (!def)
            return StepFault::untyped_unit;
        if (self_heal_due(
                h.rules.rules().repair.healtime_self_heal,
                def->heal_time,
                u.health,
                def->max_damage,
                u.build_remaining,
                w.game.tick
            ))
            h.regenerate_health(u);
        if (const auto fault = primary_orders(w, q, u, h); fault != StepFault::none)
            return fault;
        if (const auto fault = secondary_orders(w, q, u, h); fault != StepFault::none)
            return fault;
        if (u.movement) {
            h.movement_tick(u);
            if (const auto fault = update_height(w, u, h); fault != StepFault::none)
                return fault;
        }
    }
    if (u.flags & OA_UNIT_FLAG_DEATH_PENDING)
        h.kill_unit(u, u.damage_kind);
    return StepFault::none;
}

StepFault update_units(oa::World& w, std::span<OrderQueue> orders, Host& h) {
    if (orders.size() != w.unit_slot_count)
        return StepFault::orders_short;
    w.game.active_unit_count = 0;
    // A unit's fault ends that unit's update only; the pass goes on with the
    // other units and returns the first fault.
    auto first_fault = StepFault::none;
    for (auto& p : w.game.players) {
        if (!player_active(p))
            continue;
        uint32_t count = 0;
        auto* units = oa::world_player_units(&w, &p, &count);
        for (uint32_t i = 0; i < count; ++i) {
            auto& u = units[i];
            if (u.type_index) {
                ++w.game.active_unit_count;
                h.update_wind_generator(u);
                if (p.in_use && (p.status == 1 || p.status == 2))
                    h.tick_weapon_aim(u);
                const auto fault = update_unit(w, orders[oa::world_unit_slot(&w, &u)], u, h);
                if (first_fault == StepFault::none)
                    first_fault = fault;
            }
        }
        if ((w.game.session_flags & live_multiplayer_game) && p.in_use &&
            (p.status == 1 || p.status == 2))
            h.local_player_ticked(p);
    }
    if ((w.game.periodic_flags & periodic_enabled) && !h.is_key_down(0xf9)) {
        w.game.periodic_countdown = std::bit_cast<int16_t>(
            static_cast<uint16_t>(static_cast<uint16_t>(w.game.periodic_countdown) - 1u)
        );
        if (w.game.periodic_countdown < 1) {
            w.game.periodic_countdown = 90;
            h.select_next_viewpoint_unit();
            h.follow_next_selected(0);
        }
    }
    return first_fault;
}
} // namespace oa::sim::simulation_state
