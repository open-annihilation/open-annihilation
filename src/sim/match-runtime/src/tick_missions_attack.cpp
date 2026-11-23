// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Attack, patrol, guard and teleport orders of the ground block of the
// mission table.
#include "ground_missions.hpp"

namespace oa::sim::match_runtime {

namespace {
namespace attack {

constexpr uint32_t patrol_step_wait = 0xf;
constexpr uint32_t patrol_idle_wait = 0x1e;
constexpr uint32_t patrol_idle_jitter = 0x1e;
constexpr uint32_t chase_recheck_wait = 0x1e;
constexpr uint32_t guard_follow_wait = 0x1e;
constexpr uint32_t teleport_trail_ticks = 0x1e;
constexpr uint8_t special_weapon_slot = 2;
constexpr uint8_t all_weapons = 3;
// Beyond this height difference (16.16) a chaser stops circling the target
// and closes on it at half range.
constexpr int32_t chase_height_limit = 0x80000;
constexpr uint16_t chase_arc = 0x8000;
constexpr uint16_t chase_arc_offset = 0x4000;
constexpr int32_t guard_footprint_margin = 2;
constexpr int32_t world_units_per_footprint = 16;
constexpr uint8_t mobile_build_kind = 25;
constexpr uint8_t building_build_kind = 12;

// Chase approach steps (the order's second parameter): circle at range, then
// four random points on the near half at range, circle at half range, at the
// target, then two rings.
constexpr int32_t approach_circle = 0;
constexpr int32_t approach_flank_last = 4;
constexpr int32_t approach_half_range = 5;
constexpr int32_t approach_close = 6;
constexpr int32_t approach_ring = 7;
constexpr int32_t approach_wide_ring = 8;

AttackPoint attack_point(const sim::ground_orders::Point& point) {
    return {
        std::bit_cast<uint32_t>(point[0]),
        std::bit_cast<uint32_t>(point[1]),
        std::bit_cast<uint32_t>(point[2])
    };
}

} // namespace attack
} // namespace

uint32_t TickHost::GroundMissions::attack_special() {
    const auto kind = AttackAdapter(host, s, record).morph_attack_command(target());
    morph(kind);
    record.attack.weapon_slot = attack::special_weapon_slot;
    record.extra.tolerance = attack::special_weapon_slot;
    return ground::keep_waiting;
}

uint32_t TickHost::GroundMissions::patrol() {
    switch (order.phase) {
    case 0:
        if (!s.record.movement)
            return ground::mission_invalid;
        announce();
        clone_patrol();
        wait_ticks(1);
        return ground::next_phase;
    case 1:
        AttackAdapter(host, s, record).reset_weapons();
        circle_goal(record.extra.destination, 0);
        wait_ticks(attack::patrol_step_wait);
        order.wait_events |= ground::goal_events;
        return ground::next_phase;
    case 2: {
        uint32_t result = ground::rotate_mission;
        if (!(events & ground::goal_events)) {
            auto* enemy = host.match.find_automatic_target(*s.unit);
            if (enemy && host.match.issue_automatic_attack(*s.unit, *enemy)) {
                order.wait_events = 0;
                result = ground::retry_later;
            } else {
                wait_ticks(random(attack::patrol_idle_jitter) + attack::patrol_idle_wait);
                result = ground::phase_chosen;
            }
        }
        order.phase = 1;
        return result;
    }
    default:
        return ground::mission_invalid;
    }
}

uint32_t TickHost::GroundMissions::attack_chase() {
    auto& slot = record.attack.weapon_slot;
    auto& approach = record.attack.retry;
    const auto weapon = static_cast<uint8_t>(slot);
    auto* aimed = target();
    if (!aimed || (events & ground::attack_abort_events))
        return ground::mission_done;
    if (record.attack.leash != 0) {
        const auto strayed = static_cast<int32_t>(base::game_math::distance(
            static_cast<int16_t>(s.record.position.x >> 16) - record.attack.origin[0],
            static_cast<int16_t>(s.record.position.z >> 16) - record.attack.origin[1]
        ));
        if (record.attack.leash <= strayed)
            return ground::mission_done;
    }
    AttackAdapter weapons(host, s, record);
    // Before aiming the ordered slot, 3.1c occupies slots 0 and 2. Under
    // orders.selective-weapon-occupy the stage it names occupies only the
    // ordered slot, read as 2 for a slot past 1 and 0 otherwise, so slot 1
    // is never occupied and an order for it occupies slot 0.
    const auto engage = [&](bool first_stage) {
        using Attack = data::match_rules::OrdersSelectiveWeaponOccupyAttack;
        const auto occupy = rules().orders.selective_weapon_occupy.attack;
        if (occupy == Attack::ordered_slot_both_stages ||
            (first_stage && occupy == Attack::ordered_slot_first_stage)) {
            weapons.release_weapon_targets(slot > 1 ? attack::special_weapon_slot : 0);
        } else {
            weapons.release_weapon_targets(0);
            weapons.release_weapon_targets(2);
        }
        weapons.assign_target(*aimed, slot);
    };
    const auto at_target = ground::position_of(aimed->record);
    switch (order.phase) {
    case 0:
        if (!has_movement_object() || can_fly() || !(s.record.flags & OA_UNIT_FLAG_HAS_WEAPONS))
            return ground::mission_invalid;
        announce();
        record.extra.destination = ground::position_of(s.record);
        record.attack.destination = attack::attack_point(record.extra.destination);
        approach = 0;
        if (slot == 0)
            slot = weapons.selected_weapon();
        return ground::next_phase;
    case 1:
        install_goal(nullptr);
        if (events & ground::weapon_out_of_range_events)
            return ground::next_phase;
        if (!weapons.can_reach(*aimed, weapon))
            return ground::next_phase;
        engage(true);
        order.wait_events = ground::attack_wait;
        return ground::keep_waiting;
    case 2: {
        const auto range = weapons.range(weapon);
        if (approach < 0)
            return ground::mission_invalid;
        if (approach == attack::approach_circle) {
            circle_goal(at_target, range);
            ++approach;
        } else if (approach <= attack::approach_flank_last) {
            const auto rise = ground::sub_fixed(s.record.position.y, aimed->record.position.y);
            const auto height = static_cast<int32_t>(
                rise < 0 ? 0u - static_cast<uint32_t>(rise) : static_cast<uint32_t>(rise)
            );
            if (height > attack::chase_height_limit) {
                circle_goal(at_target, range / 2);
                approach = attack::approach_close;
                return ground::next_phase;
            }
            const auto bearing = ground::bearing_between(at_target, ground::position_of(s.record));
            const auto angle = static_cast<uint16_t>(
                bearing + random(attack::chase_arc) - attack::chase_arc_offset
            );
            const auto reach = static_cast<int32_t>(static_cast<uint32_t>(range) << 16);
            const sim::ground_orders::Point flank{
                ground::sub_fixed(at_target[0], sim::unit_movement::sine_scaled(angle, reach)),
                at_target[1],
                ground::sub_fixed(at_target[2], sim::unit_movement::cosine_scaled(angle, reach))
            };
            circle_goal(flank, range / 4);
        } else if (approach == attack::approach_half_range) {
            circle_goal(at_target, range / 2);
            ++approach;
        } else if (approach == attack::approach_close) {
            circle_goal(at_target, 0);
            ++approach;
        } else if (approach == attack::approach_ring) {
            ring_goal(at_target, range, range / 2);
            ++approach;
        } else if (approach == attack::approach_wide_ring) {
            ring_goal(at_target, range * 2, range);
            approach = attack::approach_circle;
        } else
            return ground::mission_invalid;
        return ground::next_phase;
    }
    case 3:
        if (events & ground::chase_wake_events) {
            order.phase = 1;
            return ground::phase_chosen;
        }
        if (weapons.can_reach(*aimed, weapon)) {
            engage(false);
            order.wait_events = ground::attack_in_range_wait;
        } else {
            weapons.reset_weapons();
            order.wait_events = ground::attack_out_of_range_wait;
        }
        wait_ticks(attack::chase_recheck_wait);
        return ground::keep_waiting;
    default:
        return ground::mission_invalid;
    }
}

uint32_t TickHost::GroundMissions::suppress() {
    if (events & ground::suppress_abort_event)
        return ground::mission_done;
    const auto weapon = static_cast<uint8_t>(record.attack.weapon_slot);
    auto& reach = record.attack.retry;
    AttackAdapter weapons(host, s, record);
    const auto point = attack::attack_point(record.extra.destination);
    switch (order.phase) {
    case 0:
        if (can_fly())
            return ground::mission_failed;
        announce();
        reach = weapons.range(weapon);
        return ground::next_phase;
    case 1: {
        int32_t fired = 1;
        if (record.attack.weapon_slot == attack::special_weapon_slot) {
            // orders.selective-weapon-occupy suppress: slot-2 occupies the
            // third slot alone, leaving the others to pick their own targets.
            using Suppress = data::match_rules::OrdersSelectiveWeaponOccupySuppress;
            weapons.release_weapon_targets(
                rules().orders.selective_weapon_occupy.suppress == Suppress::slot_2
                    ? attack::special_weapon_slot
                    : attack::all_weapons
            );
            fired = attack::special_weapon_slot;
        } else {
            weapons.release_weapon_targets(0);
            weapons.release_weapon_targets(1);
            weapons.assign_ground(point, 0);
        }
        weapons.assign_ground(point, fired);
        order.wait_events = ground::attack_ground_wait;
        return ground::next_phase;
    }
    case 2:
        weapons.reset_weapons();
        if (events & ground::suppress_rotate_event) {
            order.phase = 1;
            return ground::rotate_mission;
        }
        if (!s.record.movement || reach < 1)
            return ground::retry_mission;
        circle_goal(record.extra.destination, reach);
        order.wait_events = ground::goal_events;
        reach -= static_cast<int32_t>(random(static_cast<uint32_t>(weapons.range(weapon) / 3)));
        order.phase = 1;
        return ground::phase_chosen;
    default:
        return ground::mission_invalid;
    }
}

uint32_t TickHost::GroundMissions::follow() {
    auto* guarded = target();
    if (!guarded)
        return ground::mission_done;
    if (s.record.attach_parent)
        return ground::mission_invalid;
    if (def_of(guarded->record).flags & OA_UNIT_DEF_FLAG_CAN_FLY)
        return ground::mission_failed;
    auto& spacing = record.extra.tolerance;
    AttackAdapter weapons(host, s, record);
    if (order.phase == 0) {
        announce("Guarding");
        weapons.reset_weapons();
        const auto cells = static_cast<int32_t>(s.record.footprint_x) +
                           attack::guard_footprint_margin +
                           static_cast<int32_t>(guarded->record.footprint_x);
        spacing = cells * attack::world_units_per_footprint;
        const auto reach = static_cast<int32_t>(static_cast<uint32_t>(spacing) << 16);
        const auto angle = static_cast<uint16_t>(random(0x10000));
        record.extra.destination = {
            -sim::unit_movement::sine_scaled(angle, reach),
            0,
            -sim::unit_movement::cosine_scaled(angle, reach)
        };
        return ground::next_phase;
    }
    if (order.phase != 1)
        return ground::mission_invalid;
    auto* attacker = unit_at(static_cast<int32_t>(guarded->record.last_attacker_id));
    if (attacker && !allied(attacker->record.owner_index, s.record.owner_index) &&
        (events & ground::target_attacked_event)) {
        const auto* masks = host.match.fields(s).target_masks;
        const auto attacker_type = static_cast<uint16_t>(attacker->record.type_index);
        if (!masks || !masks->no_chase.contains(attacker_type)) {
            if (host.match.issue_attack(s.unit_index, host.slot(*attacker).unit_index, true)) {
                order.wait_events = 0;
                return ground::retry_later;
            }
            if (s.record.flags & OA_UNIT_FLAG_FIRE_ORDER_MASK) {
                for (uint8_t slot = 0; slot < OA_UNIT_WEAPON_COUNT; ++slot) {
                    const auto flags = s.record.weapons[slot].flags;
                    const auto* weapon = oa::world_weapon_def(&world(), s.record.weapons[slot].def);
                    if (!(flags & OA_UNIT_WEAPON_ENABLED) || !(flags & OA_UNIT_WEAPON_RETALIATE) ||
                        !weapon || (weapon->flags & OA_WEAPON_FLAG_COMMAND_FIRE))
                        continue;
                    auto* current = weapon_target(slot);
                    bool retarget_slot = !current || !weapons.can_reach(*current, slot);
                    if (!retarget_slot && masks) {
                        const auto type = static_cast<uint16_t>(current->record.type_index);
                        retarget_slot = slot == 0   ? masks->primary_bad.contains(type)
                                        : slot == 1 ? masks->secondary_bad.contains(type)
                                                    : masks->special_bad.contains(type);
                    }
                    if (retarget_slot)
                        weapons.assign_target(*attacker, slot);
                }
            }
        }
    }
    const bool builder = (def().flags & OA_UNIT_DEF_FLAG_BUILDER) != 0;
    const auto& guarded_def = def_of(guarded->record);
    if (static_cast<uint32_t>(static_cast<int32_t>(guarded->record.health)) <
            guarded_def.max_damage &&
        builder) {
        if (const auto kind = repair_command_kind(*guarded)) {
            install_goal(nullptr);
            push_order_front(create_order(kind, guarded, nullptr, 0, 0, 0));
            order.wait_events = 0;
            return ground::retry_later;
        }
    }
    auto* busy = guarded->primary;
    if (busy && busy->kind != 0 && builder && (guarded_def.flags & OA_UNIT_DEF_FLAG_BUILDER) &&
        (busy->flags & ground::order_assistable)) {
        auto& work = host.owned(*busy);
        auto* work_target =
            work.construction.target ? work.construction.target : work.attack.target;
        if (work_target != s.unit) {
            const bool construction = busy->kind == attack::mobile_build_kind ||
                                      busy->kind == attack::building_build_kind;
            const bool aimed = (work.extra.command_flags & ground::order_has_target) && work_target;
            const bool placed = (work.extra.command_flags & ground::order_has_point) != 0;
            if (construction ? work_target != nullptr : (aimed || placed)) {
                install_goal(nullptr);
                const auto kind = construction ? ground::help_build_kind : busy->kind;
                const auto point = work.extra.destination;
                push_order_front(create_order(kind, work_target, &point, 0, 0, 0));
                order.wait_events = 0;
                return ground::retry_later;
            }
        }
    }
    // orders.con-patrol-guard-options: a stay or scatter choice rewrites the
    // whole parts of the offset's x and z (their fractions stay) to 7/20 of
    // the spacing or the whole spacing, each signed toward the side of the
    // guarded unit the guard stands on. Every guarding ground unit takes it,
    // builder or not, and the order keeps the rewritten offset.
    if (const auto choice = guard_choice(); choice != ground::guard_base) {
        const auto whole = static_cast<uint32_t>(spacing);
        const auto distance =
            static_cast<uint16_t>(choice == ground::guard_stay ? whole * 7u / 20u : whole);
        const auto side = [](int32_t guard, int32_t guarded) {
            return static_cast<int32_t>(static_cast<uint16_t>(static_cast<uint32_t>(guard) >> 16)) -
                   static_cast<int32_t>(
                       static_cast<uint16_t>(static_cast<uint32_t>(guarded) >> 16)
                   );
        };
        const auto signed_distance = [&](int32_t toward) {
            return toward < 0 ? static_cast<uint16_t>(0u - distance) : distance;
        };
        auto& rewritten = record.extra.destination;
        const auto with_whole = [](int32_t value, uint16_t whole_part) {
            return static_cast<int32_t>(
                (static_cast<uint32_t>(value) & 0xffffu) | (static_cast<uint32_t>(whole_part) << 16)
            );
        };
        rewritten[0] = with_whole(
            rewritten[0], signed_distance(side(s.record.position.x, guarded->record.position.x))
        );
        rewritten[2] = with_whole(
            rewritten[2], signed_distance(side(s.record.position.z, guarded->record.position.z))
        );
    }
    const auto& offset = record.extra.destination;
    const sim::ground_orders::Point beside{
        wrapping_add(guarded->record.position.x, offset[0]),
        wrapping_add(guarded->record.position.y, offset[1]),
        wrapping_add(guarded->record.position.z, offset[2])
    };
    circle_goal(beside, spacing / 2);
    wait_ticks(attack::guard_follow_wait);
    order.wait_events |= ground::guard_wait_events;
    return ground::keep_waiting;
}

uint32_t TickHost::GroundMissions::teleport() {
    const auto* bounds = host.match.bounds_for(*s.unit);
    if (!bounds) {
        host.match.fault_.note("teleporter without type bounds");
        return ground::mission_failed;
    }
    const auto& at = s.record.position;
    const auto& to = record.extra.destination;
    const int32_t minimum[3]{
        wrapping_add(at.x, bounds->bounds_min_x),
        wrapping_add(at.y, bounds->bounds_min_y),
        wrapping_add(at.z, bounds->bounds_min_z)
    };
    const int32_t maximum[3]{
        wrapping_add(at.x, bounds->bounds_max_x),
        wrapping_add(at.y, bounds->model_height),
        wrapping_add(at.z, bounds->bounds_max_z)
    };
    // Empty slots are passed over; 3.1c also moves the records of dead units.
    for (auto& other : host.match.slots_) {
        if (!other.unit || other.unit == s.unit || other.record.type_index == 0)
            continue;
        const auto& p = other.record.position;
        if (p.x < minimum[0] || p.x > maximum[0] || p.z < minimum[2] || p.z > maximum[2] ||
            p.y < minimum[1] || p.y > maximum[1])
            continue;
        const int32_t moved[3]{
            wrapping_sub(wrapping_add(to[0], p.x), at.x),
            wrapping_sub(wrapping_add(to[1], p.y), at.y),
            wrapping_sub(wrapping_add(to[2], p.z), at.z)
        };
        sim::effect_particles::spawn_teleport_trail(
            host.match.effects(),
            world().game,
            host.match.effect_host(),
            {p.x, p.y, p.z},
            {moved[0], moved[1], moved[2]},
            attack::teleport_trail_ticks,
            sim::effect_particles::layer_teleport
        );
        host.match.place_unit(other, moved[0], moved[1], moved[2]);
    }
    return ground::mission_done;
}

} // namespace oa::sim::match_runtime
