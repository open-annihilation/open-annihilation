// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "tick_internal.hpp"

#include <cstdint>

namespace oa::sim::match_runtime {

void TickHost::AttackAdapter::set_goal(std::unique_ptr<sim::ground_orders::Goal> goal) {
    auto& g = host.ground(*source.unit);
    g.project_slot();
    auto flags = host.weapon_flags(source);
    if (record.extra.goal) {
        sim::ground_orders::install_goal(
            host.view(source, g, flags), nullptr, host.match.simulation_.tick, host
        );
        record.extra.goal.reset();
    }
    if (goal) {
        record.order.raised_events &= ~sim::ground_orders::goal_event_mask;
        sim::ground_orders::install_goal(
            host.view(source, g, flags), goal.get(), host.match.simulation_.tick, host
        );
        record.extra.goal = std::move(goal);
    }
    host.write_flags(source, flags);
}

void TickHost::AttackAdapter::announce(const char* caption) {
    if (record.extra.command_flags & command_unqueued) {
        record.extra.command_flags &= static_cast<uint8_t>(~command_unqueued);
        host.play_sound(*source.unit, 5, caption);
    }
}

uint8_t TickHost::AttackAdapter::selected_weapon() {
    const auto flags = host.weapon_flags(source);
    if (flags[0] & OA_UNIT_WEAPON_ENABLED)
        return 0;
    if (flags[1] & OA_UNIT_WEAPON_ENABLED)
        return 1;
    return flags[2] & OA_UNIT_WEAPON_ENABLED;
}

void TickHost::AttackAdapter::release_weapon_targets(uint32_t i) {
    if (i > 3) {
        host.match.fault_.note("attack weapon index outside slots");
        return;
    }
    host.before_callback();
    host.match.release_tracked_weapons(source, static_cast<uint8_t>(i));
    host.after_callback();
}

void TickHost::AttackAdapter::assign_target(sim::simulation_state::Unit& target, int32_t index) {
    if (index < 0 || index >= 3) {
        host.match.fault_.note("attack weapon index outside slots");
        return;
    }
    sim::weapon_execution::aim_slot_at_unit(
        source.record, target.record, static_cast<uint8_t>(index)
    );
}

void TickHost::AttackAdapter::reset_weapons() {
    host.before_callback();
    host.match.bridge_->reset_weapon_targets(source.record, 3);
    host.after_callback();
}

bool TickHost::AttackAdapter::can_reach(sim::simulation_state::Unit& target, uint8_t index) {
    return host.match.weapon_can_reach(source.unit_index, host.slot(target).unit_index, index);
}

int32_t TickHost::AttackAdapter::range(uint8_t index) {
    return host.match.weapons_[source.unit_index].definitions.at(index)->range_world_units;
}

void TickHost::AttackAdapter::clear_goal() {
    set_goal(nullptr);
}

void TickHost::AttackAdapter::circle_goal(const AttackPoint& point, int32_t radius) {
    auto& g = host.ground(*source.unit);
    g.project_slot();
    sim::ground_orders::Point p;
    for (std::size_t i = 0; i < 3; ++i)
        p[i] = std::bit_cast<int32_t>(point[i]);
    set_goal(
        std::make_unique<sim::ground_orders::Goal>(
            sim::ground_orders::make_goal(record.order, g.geometry, p, radius)
        )
    );
}

uint32_t TickHost::AttackAdapter::random(uint32_t limit) {
    return host.random(limit);
}

uint8_t TickHost::AttackAdapter::morph_attack_command(sim::simulation_state::Unit* target) {
    const auto& definition = *host.match.fields(source).definition;
    const auto& weapons = host.match.weapons_[source.unit_index];
    const auto weapon_flags = [](const sim::combat_state::WeaponDefinition* weapon) {
        return weapon ? weapon->flags : 0u;
    };
    CommandSource projected;
    projected.can_move = definition.can_move;
    projected.can_attack = definition.can_attack;
    // Unit.movement: only units with a movement runtime have the movement
    // object.
    projected.object_present = host.match.ground_runtime(source.unit_index) != nullptr;
    projected.unit_flags = source.unit->flags;
    projected.type_flags = source.unit->type->flags;
    projected.primary_weapon_flags = weapon_flags(weapons.definitions[0]);
    projected.secondary_weapon_flags = weapon_flags(weapons.definitions[1]);
    projected.type_primary_weapon_flags = weapon_flags(weapons.definitions[0]);
    projected.secondary_slot_flags = source.record.weapons[1].flags;
    if (const auto* primary = weapons.definitions[0])
        projected.primary_surface_fire =
            host.match.rules_view()
                .weapon(oa::oa_weapon_def_id(&host.match.weapon_record(*primary)))
                .surface_fire;
    std::optional<CommandTarget> other;
    if (target) {
        auto& to = host.slot(*target);
        bool allied = false;
        if (source.unit->owner && target->owner) {
            const auto from = source.unit->owner->record.index;
            const auto dest = target->owner->record.index;
            if (from < 10 && dest < 10 && host.match.player_alliances_[from])
                allied = (*host.match.player_alliances_[from])[dest] != 0;
        }
        CommandTarget cmd;
        cmd.allied = allied;
        cmd.unit_flags = target->flags;
        cmd.type_flags = target->type->flags;
        cmd.height = static_cast<int16_t>(target->position[1] >> 16);
        cmd.model_height = static_cast<int16_t>(
            static_cast<uint32_t>(match_unit_def(host.match, to.record).model_height) >> 16
        );
        other = cmd;
    }
    const auto name = resolve_combat_command(3, projected, other, host.match.simulation_.sea_level);
    const auto kind = combat_order_kind(name);
    record.order.kind = kind;
    const auto desc = kind == suppress_kind         ? 0x410u
                      : kind == attack_special_kind ? 0x680u
                                                    : 0x280u;
    auto packed = static_cast<uint32_t>(record.order.preserve_flags) |
                  (static_cast<uint32_t>(record.extra.command_flags) << 8) |
                  (static_cast<uint32_t>(record.order.flags) << 16);
    packed = ((packed ^ desc) & 0x600u) ^ desc;
    record.order.preserve_flags = static_cast<uint8_t>(packed);
    record.extra.command_flags = static_cast<uint8_t>(packed >> 8);
    record.order.flags = static_cast<uint8_t>(packed >> 16);
    return kind;
}

void TickHost::AttackAdapter::assign_ground(const AttackPoint& point, int32_t slot) {
    if (slot < 0 || slot >= 3) {
        host.match.fault_.note("ground weapon index outside slots");
        return;
    }
    sim::weapon_execution::aim_slot_at_point(
        source.record,
        {std::bit_cast<oa::oa_fixed>(point[0]),
         std::bit_cast<oa::oa_fixed>(point[1]),
         std::bit_cast<oa::oa_fixed>(point[2])},
        static_cast<uint8_t>(slot)
    );
}

void TickHost::PatrolAdapter::clone_patrol() {
    constexpr uint8_t cloned_mark = 0x80;
    bool already = false;
    for_each_primary_uncycled(source.unit->primary, [&](sim::simulation_state::Order* order) {
        const auto* extra = host.match.owned_extra(order);
        if (extra && (extra->command_flags & cloned_mark))
            already = true;
    });
    record.extra.command_flags |= cloned_mark;
    if (already)
        return;
    sim::ground_orders::Point here{
        signed_word(source.unit->position[0]),
        signed_word(source.unit->position[1]),
        signed_word(source.unit->position[2])
    };
    const auto kind = record.order.kind;
    (void)host.match.issue_queued_command(
        source.unit_index, kind, here, true, mission_descriptor_table.at(kind)
    );
}

} // namespace oa::sim::match_runtime
