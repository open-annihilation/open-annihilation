// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/ai.hpp"
#include "oa/sim/scenario/commander_rules.hpp"
#include "tick_internal.hpp"
#include "oa/sim/weapon_execution/projectile_pool.hpp"
#include "oa/formats/tdf.hpp"

#include <array>
#include <cassert>
#include <cstdint>

namespace oa::sim::match_runtime {

namespace {
constexpr uint32_t unit_damaged_event = 0x10;  // the damage reaction wakes the watchers with it
constexpr uint32_t attack_notice_category = 2; // sound_announce_if_offscreen
constexpr uint16_t board_pinned_flag = 0x80;   // Game.graphics_flags: kills board held open
constexpr int32_t dead_unit_sight_ticks =
    60; // the kill handler remembers the viewer's dead units this long

/// Restarts the unit's damage countdown, as every hit does.
///
/// @param[in,out] unit Unit that was hit.
void restart_damage_countdown(oa::Unit& unit) noexcept {
    unit.damage_countdown = damage_script_countdown;
}
} // namespace

bool side_commander(const oa::World& world, const oa::Unit& unit) noexcept {
    const auto* owner = oa::world_unit_owner(&world, &unit);
    const auto* info = owner != nullptr ? oa::world_player_info(&world, owner) : nullptr;
    const auto* definition = oa::world_unit_def_of(&world, &unit);
    if (info == nullptr || info->side >= OA_SIDE_COUNT || definition == nullptr)
        return false;
    return formats::tdf::compare_nocase(
               world.game.sides[info->side].commander, definition->unit_name
           ) == 0;
}

Match::RuntimeOrder* Match::runtime_order(const sim::simulation_state::Order* order) {
    if (order == nullptr)
        return nullptr;
    for (auto& entry : orders_)
        if (&entry->order == order)
            return entry.get();
    return nullptr;
}

void Match::wake_target_observers(sim::simulation_state::Unit& unit, uint32_t event) {
    const auto found = target_observers_.find(&unit);
    if (found == target_observers_.end())
        return;
    for (auto* entry = found->second; entry != nullptr; entry = entry->observer_next) {
        // Every order on the list still targets this unit. The walk raises
        // the event on each of them.
        assert(entry->attack.target == &unit);
        entry->order.raised_events |= event;
    }
}

void Match::react_to_damage(sim::unit_spawn::Slot& target, sim::unit_spawn::Slot* source) {
    wake_target_observers(*target.unit, unit_damaged_event);
    sim::weapon_execution::RetaliationHooks hooks;
    hooks.context = this;
    hooks.rules = rules_view();
    hooks.category_contains = [](void* context, oa::oa_ref32 mask, uint16_t type_index) {
        const auto& masks = static_cast<Match*>(context)->state_.category_masks;
        return mask != 0 && mask <= masks.size() && masks[mask - 1]->contains(type_index);
    };
    hooks.head_order_flags = [](void* context, const oa::Unit& unit, uint32_t* flags) {
        auto& match = *static_cast<Match*>(context);
        const auto* entry = match.runtime_order(match.units_.at(unit.id).primary);
        if (entry == nullptr)
            return false;
        *flags = static_cast<uint32_t>(entry->order.preserve_flags) |
                 static_cast<uint32_t>(entry->extra.command_flags) << 8 |
                 static_cast<uint32_t>(entry->order.flags) << 16;
        return true;
    };
    hooks.queue_attack = [](void* context, oa::Unit& victim, oa::Unit& attacker) {
        return static_cast<Match*>(context)->issue_attack(victim.id, attacker.id, false);
    };
    hooks.random = [](void* context, uint32_t limit) {
        return static_cast<Match*>(context)->random_bounded(limit);
    };
    hooks.computer_alert = [](void* context, oa::Unit& victim, uint32_t tick) {
        auto& match = *static_cast<Match*>(context);
        sim::ai::hold_capturer_builds(match, victim.owner_index, tick);
        TickHost host(match);
        match.note_step(
            sim::simulation_state::clear_orders(match.units_.at(victim.id), false, host)
        );
    };
    oa::Unit* attacker = source != nullptr && source->unit != nullptr ? &source->record : nullptr;
    const auto reaction = sim::weapon_execution::retaliate(state(), target.record, attacker, hooks);
    if (reaction.attack_notice)
        services_.command_sound(target, attack_notice_category);
}

void Match::paralyze(sim::unit_spawn::Slot& target, int16_t amount) {
    auto* head = runtime_order(target.unit->primary);
    const bool head_is_paralyze = head != nullptr && head->order.kind == paralyze_order_kind;
    switch (sim::unit_health::paralyze_action(state(), target.record, head_is_paralyze)) {
    case sim::unit_health::ParalyzeAction::extend:
        head->extra.tolerance += amount;
        break;
    case sim::unit_health::ParalyzeAction::insert:
        (void)insert_ground_order(target.unit_index, paralyze_order_kind, std::nullopt, amount);
        break;
    case sim::unit_health::ParalyzeAction::none:
        break;
    }
}

void Match::apply_damage_event(
    sim::unit_spawn::Slot& target,
    sim::unit_spawn::Slot* source,
    int16_t amount,
    uint8_t kind,
    uint8_t direction
) {
    auto& unit = *target.unit;
    if (!(unit.flags & live_unit_flag) || (unit.flags & death_pending_flag))
        return;
    if (event_wanted<&EventHooks::unit_damaged>()) {
        const auto attacker =
            source != nullptr && source->unit != nullptr ? source->unit_index : uint16_t{};
        emit_event<&EventHooks::unit_damaged>(state(), target.unit_index, attacker, amount, kind);
    }
    if (kind == healing_kind) {
        if (unit.type)
            unit.health =
                sim::unit_health::healed_health(unit.health, amount, unit.type->maximum_health);
        return;
    }
    restart_damage_countdown(target.record);
    if (kind != non_relayed_kind)
        react_to_damage(target, source);
    unit.record.damage_kind = kind;
    if (source && source->unit) {
        target.record.last_attacker_owner = source->record.owner_index;
        target.record.last_attacker_id = source->unit_index;
        if (combat_activity.hit)
            combat_activity.hit(
                combat_activity.context, source->record.owner_index, target.record.owner_index
            );
    }
    if (kind == paralyze_kind) {
        paralyze(target, amount);
        return;
    }
    unit.health =
        static_cast<int16_t>(static_cast<uint16_t>(unit.health) - static_cast<uint16_t>(amount));
    if (unit.health < 1) {
        if (sim::simulation_state::locally_simulated(unit)) {
            unit.flags |= death_pending_flag;
            return;
        }
        unit.health = 0;
    }
    if (kind == weapon_hit_kind) {
        if (auto* instance = this->instance(target.unit_index); instance && instance->script()) {
            const auto heading = static_cast<uint16_t>(static_cast<uint16_t>(direction) << 8);
            const std::array<int32_t, 2> hit{
                sim::unit_movement::cosine_scaled(heading, hit_script_scale),
                sim::unit_movement::sine_scaled(heading, hit_script_scale)
            };
            instance->script()->call("HitByWeapon", hit, false);
            auto percent = 0;
            if (unit.type && unit.type->maximum_health) {
                percent = static_cast<int32_t>(
                    static_cast<uint32_t>(static_cast<int32_t>(unit.health) * 100) /
                    unit.type->maximum_health
                );
                if (percent < 0)
                    percent = 0;
                if (percent > 100)
                    percent = 100;
            }
            const std::array<int32_t, 1> taken{percent};
            instance->script()->call("TakeDamage", taken, false);
        }
    }
}

void Match::capture_unit(sim::unit_spawn::Slot& original, sim::unit_spawn::Slot& capturer) {
    if (!original.unit || !capturer.unit || capturer.unit->owner == nullptr)
        return;
    transfer_unit(original.unit_index, capturer.record.owner_index);
}

void Match::transfer_unit(uint16_t unit, uint8_t new_owner, const TransferredUnit* carried) {
    auto& original = slots_.at(unit);
    if (!original.unit || new_owner >= simulation_.players.size())
        return;
    // Read before the captured event or the copy can change the record.
    const uint8_t from = original.record.owner_index;
    auto& receiver = simulation_.players[new_owner];
    if (original.unit->owner == &receiver)
        return;
    if (!(original.unit->flags & live_unit_flag) || (original.unit->flags & death_pending_flag))
        return;
    if (!receiver.present)
        return;
    constexpr auto captured = static_cast<uint8_t>(DeathKind::captured);
    // The captured event is dispatched with the captured unit (owner 1)
    // before the transfer destroys it.
    scenario_unit_captured(original);
    const bool simulated_here = sim::simulation_state::locally_simulated(*original.unit);
    if (simulated_here && receiver.status == OA_PLAYER_STATUS_MIRRORED) {
        // The receiving player's machine creates the unit; here it dies.
        constexpr uint32_t handed_over_cooldown = 150; // ticks it cannot be selected
        original.unit->flags &= ~OA_UNIT_FLAG_SELECTED;
        original.unit->capture_cooldown = handed_over_cooldown;
        if (multiplayer.unit_transferred != nullptr)
            multiplayer.unit_transferred(multiplayer.context, unit, new_owner);
        emit_event<&EventHooks::unit_given>(state(), unit, uint16_t{0}, from, new_owner);
        TickHost(*this).scaled_damage(nullptr, original, lethal_damage, captured);
        return;
    }
    if (receiver.status != OA_PLAYER_STATUS_LOCAL && receiver.status != OA_PLAYER_STATUS_COMPUTER)
        return;
    if (!original.unit->record.type_index)
        return;
    sim::unit_spawn::Request request;
    request.player = new_owner;
    request.type = original.unit->record.type_index;
    request.position = original.unit->position;
    request.finished = true;
    request.state = original.unit->flags & OA_UNIT_FLAG_OCCUPANCY_MASK;
    // A building handed over keeps its facing (units.build-rotation).
    request.facing = unit_build_facing(original.record);
    auto* copy = create(request);
    if (!copy || !copy->unit)
        return;
    // The copy drops the standing move and fire orders it was created with.
    copy->unit->flags &= ~(OA_UNIT_FLAG_MOVE_ORDER_MASK | OA_UNIT_FLAG_FIRE_ORDER_MASK);
    emit_event<&EventHooks::unit_given>(state(), unit, copy->unit_index, from, new_owner);
    if (carried != nullptr) {
        // The unit's own machine kills the old unit and shares its death.
        copy->unit->health = carried->health;
        copy->record.build_remaining = carried->build_remaining;
        copy->record.bank = carried->bank;
        copy->yaw = carried->heading;
        copy->record.pitch = carried->pitch;
        for (std::size_t i = 0; i < OA_UNIT_WEAPON_COUNT; ++i)
            if (copy->record.weapons[i].flags & OA_UNIT_WEAPON_ENABLED)
                copy->record.weapons[i].stockpile = carried->stockpiles[i];
    } else {
        copy->unit->health = original.unit->health;
        copy->record.build_remaining = original.record.build_remaining;
        copy->record.bank = original.record.bank;
        copy->yaw = original.yaw;
        copy->record.pitch = original.record.pitch;
        for (std::size_t i = 0; i < OA_UNIT_WEAPON_COUNT; ++i)
            if (copy->record.weapons[i].flags & OA_UNIT_WEAPON_ENABLED)
                copy->record.weapons[i].stockpile = original.record.weapons[i].stockpile;
        TickHost(*this).scaled_damage(nullptr, original, lethal_damage, captured);
    }
    set_activation(*copy, original.record.state_flags, true);
    set_activation(*copy, static_cast<uint8_t>(~original.record.state_flags), false);
}

void Match::spawn_corpse(sim::unit_spawn::Slot& slot, uint8_t wreck_level, bool smoke) {
    constexpr int32_t wreck_smoke_interval = 15;
    constexpr int32_t wreck_smoke_duration = 900;
    auto& world = state();
    auto& unit = slot.record;
    const auto* def = oa::world_unit_def_of(&world, &unit);
    if (def == nullptr)
        return;
    auto corpse = static_cast<uint16_t>(def->corpse);
    for (; wreck_level >= 2; --wreck_level) {
        if (corpse >= OA_PLOT_FEATURE_RESERVED || corpse >= world.feature_def_count)
            return;
        corpse = world.feature_defs[corpse].dead_feature;
    }
    if (corpse >= OA_PLOT_FEATURE_RESERVED)
        return;
    const auto* plot = oa::world_plot(&world, unit.cell_x, unit.cell_z);
    if (plot == nullptr)
        return;
    const auto index = static_cast<std::size_t>(plot - world.plots);
    const int16_t orientation[3] = {unit.bank, static_cast<int16_t>(unit.heading), unit.pitch};
    const auto ground =
        sim::feature_runtime::sample_height(world, unit.position.x, unit.position.z);
    auto* placed = sim::feature_runtime::place_feature(
        world, feature_host(), index, corpse, &unit.position, orientation, unit.owner_index
    );
    // At or under sea level a 3DO corpse sinks, unless the unit is itself a
    // feature, and does not smoke.
    if (ground <= static_cast<int32_t>(static_cast<uint8_t>(world.game.sea_level)) &&
        placed != nullptr) {
        if ((def->flags & OA_UNIT_DEF_FLAG_IS_FEATURE) == 0) {
            placed->model.velocity.z = 0;
            placed->model.velocity.y = sim::feature_runtime::wreck_sink_speed;
        }
        smoke = false;
    }
    // unit_died has already fired, and the dead unit's record still holds
    // its position. A placement that returns no feature leaves no wreck.
    if (placed != nullptr)
        emit_event<&EventHooks::wreck_placed>(
            world, slot.unit_index, corpse, static_cast<uint32_t>(index)
        );
    if (world.plots[index].feature == corpse)
        wrecks_.push_back(
            {corpse,
             slot.unit->position,
             unit.bank,
             slot.yaw,
             unit.pitch,
             static_cast<int32_t>(unit.cell_x),
             static_cast<int32_t>(unit.cell_z)}
        );
    if (smoke)
        sim::effect_particles::spawn_smoke_column(
            effects(),
            world.game,
            effect_host(),
            unit.position,
            wreck_smoke_interval,
            wreck_smoke_duration,
            sim::effect_particles::layer_smoke
        );
}

void Match::record_death_statistics(
    sim::unit_spawn::Slot& slot, DeathKind death, sim::unit_spawn::Slot* killer
) {
    constexpr uint8_t no_player = 10;
    auto& world = state();
    Player* owner = world_player(&world, slot.record.owner_index);
    const auto killer_player = slot.record.last_attacker_owner;
    const bool finished = std::bit_cast<uint32_t>(slot.record.build_remaining) == 0;
    const auto* definition = fields(slot).definition;
    const bool commander = side_commander(world, slot.record);
    bool counted = false;
    if (death == DeathKind::self_destruct) {
        // Own losses only, unless the local player's row marks the owner.
        const Player* local = world_player(&world, world.game.local_player_index);
        if (owner != nullptr &&
            !(local != nullptr && owner->index < sizeof(local->economy_processed) &&
              local->economy_processed[owner->index] != 0)) {
            counted = true;
            ++owner->losses;
            if (commander)
                ++owner->commanders_lost;
        }
    } else if (
        death == DeathKind::weapon || death == DeathKind::cargo ||
        (death == DeathKind::reclaim && killer_player != no_player &&
         killer_player != slot.record.owner_index)
    ) {
        if (owner != nullptr) {
            counted = true;
            ++owner->losses;
            Player* scorer =
                killer_player != no_player ? world_player(&world, killer_player) : nullptr;
            if (scorer != nullptr && finished && slot.record.owner_index != killer_player)
                ++scorer->kills;
            if (commander) {
                if (scorer != nullptr)
                    ++scorer->commanders_killed;
                ++owner->commanders_lost;
            }
            if (killer != nullptr && finished && slot.record.owner_index != killer_player)
                ++killer->record.veteran_level;
            if (combat_activity.kill)
                combat_activity.kill(combat_activity.context, killer_player);
        }
    }
    // Skirmish and multiplayer games rank the killer on the kills board.
    if (counted && killer_player != no_player) {
        Player* scorer = world_player(&world, killer_player);
        if (scorer != nullptr && !campaign_outcomes_ &&
            sim::simulation_state::player_active(*scorer) &&
            sim::scenario::promote_on_kill_board(world, *scorer) && kill_board.took_lead)
            kill_board.took_lead(
                kill_board.context, killer_player, sim::scenario::board_score(world.game, *scorer)
            );
        if ((world.game.graphics_flags & board_pinned_flag) != 0 && kill_board.flash)
            kill_board.flash(kill_board.context, killer_player, owner->index);
    }
    // Reclaiming credits the unbuilt share of the metal cost.
    if (death == DeathKind::reclaim && killer != nullptr && definition != nullptr) {
        const auto credit =
            (1.0F - slot.record.build_remaining) * static_cast<float>(definition->build_cost_metal);
        credit_metal(killer->record, credit);
    }
}

void Match::release_target_observers(sim::simulation_state::Unit& target) {
    const auto found = target_observers_.find(&target);
    if (found == target_observers_.end())
        return;
    auto* entry = found->second;
    std::size_t steps = orders_.size();
    while (entry != nullptr && steps-- > 0) {
        auto* next = entry->observer_next;
        lose_target(*entry, target);
        entry = next;
    }
    target_observers_.erase(found);
}

void Match::lose_target(RuntimeOrder& entry, const sim::simulation_state::Unit& target) {
    constexpr uint32_t target_lost_event = 0x08;
    entry.order.raised_events |= target_lost_event;
    if (entry.attack.target == &target)
        entry.attack.target = nullptr;
    if (entry.construction.target == &target)
        entry.construction.target = nullptr;
    entry.observer_next = nullptr;
}

void Match::link_target_observer(RuntimeOrder& entry, sim::simulation_state::Unit* target) {
    // Drop the order from the unit it watches now, so that unit's list no
    // longer reaches it once the order points somewhere else.
    unlink_target_observer(entry);
    if (target == nullptr || target->record.type_index == 0) {
        entry.attack.target = nullptr;
        entry.observer_next = nullptr;
        check_target_observer(entry);
        return;
    }
    auto& head = target_observers_[target];
    entry.attack.target = target;
    entry.observer_next = head;
    head = &entry;
    check_target_observer(entry);
}

void Match::unlink_target_observer(RuntimeOrder& entry) {
    if (entry.attack.target != nullptr) {
        auto** link = &target_observers_[entry.attack.target];
        // Every order may watch one unit, so the list is never longer than
        // the orders there are; the bound only stops a damaged list looping.
        std::size_t steps = orders_.size();
        while (*link != nullptr && *link != &entry && steps-- > 0)
            link = &(*link)->observer_next;
        if (*link == &entry)
            *link = entry.observer_next;
        entry.attack.target = nullptr;
        entry.observer_next = nullptr;
    }
    check_target_observer(entry);
}

void Match::check_target_observer(const RuntimeOrder& entry) const noexcept {
#ifndef NDEBUG
    auto* target = entry.attack.target;
    for (const auto& [unit, head] : target_observers_) {
        auto* cursor = head;
        int seen = 0;
        std::size_t steps = overlay_order_budget;
        while (cursor != nullptr && steps-- > 0) {
            if (cursor == &entry)
                ++seen;
            cursor = cursor->observer_next;
        }
        if (unit != target)
            assert(seen == 0);
        else if (cursor == nullptr)
            assert(seen == 1);
        else
            assert(seen <= 1);
    }
    if (target != nullptr)
        assert(target_observers_.find(target) != target_observers_.end());
#else
    (void)entry;
#endif
}

void Match::release_air_driver(uint16_t unit) {
    auto& driver = air_drivers_.at(unit);
    if (!driver.local)
        mirrored_air_goals_.at(unit) = {};
    driver = {};
}

void Match::teardown_dead_unit(sim::unit_spawn::Slot& slot) {
    teardown_dead_unit(slot, {static_cast<DeathKind>(slot.record.damage_kind)});
}

void Match::teardown_dead_unit(sim::unit_spawn::Slot& slot, const KillOutcome& outcome) {
    teardown_dead_unit(slot, outcome, false);
}

void Match::teardown_dead_unit(
    sim::unit_spawn::Slot& slot, const KillOutcome& outcome, bool settled_elsewhere
) {
    auto& unit = *slot.unit;
    if (!(unit.flags & live_unit_flag))
        return;
    emit_event<&EventHooks::unit_died>(state(), slot.unit_index, outcome, settled_elsewhere);
    // The kill handler first leaves the viewer seeing where its own unit died.
    const auto* sight_def = oa::world_unit_def_of(&state(), &slot.record);
    const auto* sight_owner = oa::world_unit_owner(&state(), &slot.record);
    if (sight_def != nullptr && sight_owner != nullptr &&
        sight_owner->index == sight_.viewpoint_player) {
        auto context = sight_context();
        sim::visibility_state::remember_sight(
            remembered_sight_,
            slot.record.position,
            sight_def->sight_distance,
            static_cast<uint8_t>(static_cast<uint32_t>(sight_def->model_height) >> 16U),
            dead_unit_sight_ticks,
            simulation_.tick,
            context
        );
    }
    // A carried unit leaves its carrier; units it carries die with
    // it, as self-destruct when it self-destructed and as cargo deaths
    // otherwise, and are set down where they hang.
    const auto death = outcome.kind;
    sim::unit_spawn::Slot* killer = nullptr;
    if (slot.record.last_attacker_id != 0 && slot.record.last_attacker_id < slots_.size() &&
        slots_[slot.record.last_attacker_id].unit)
        killer = &slots_[slot.record.last_attacker_id];
    const auto cargo_kind = static_cast<uint8_t>(
        death == DeathKind::self_destruct ? DeathKind::self_destruct : DeathKind::cargo
    );
    if (link_parent(slot.record))
        set_carry_link(slot.unit_index, 0, -1, 1);
    while (const auto child = link_first_child(slot.record)) {
        auto& cargo = slots_.at(child);
        if (cargo.unit)
            TickHost(*this).scaled_damage(killer, cargo, lethal_damage, cargo_kind);
        set_carry_link(child, 0, -1, 1);
        // A link that does not let go ends the loop.
        if (link_first_child(slot.record) == child)
            break;
    }
    // A death settled on another player's machine plays the unit's Killed
    // script for its flying pieces; the wreck level it picks is not read.
    if (settled_elsewhere && outcome.killed_percent > 0)
        if (auto* instance = this->instance(slot.unit_index); instance && instance->script()) {
            const std::array<int32_t, 1> percent{outcome.killed_percent};
            (void)instance->script()->call("Killed", percent, true);
        }
    record_death_statistics(slot, death, killer);
    assign_squad(slot, 0xffffffffu);

    scenario_unit_destroyed(slot);
    sim::weapon_execution::cancel_burst_spawners(
        state(), oa::world_unit_ref(&state(), &slot.record)
    );
    // Every dead unit's footprint is cleared, buildings included, and the
    // unit leaves its bucket chain.
    auto& projected = begin_spatial_change(slot);
    spatial_.tick = simulation_.tick;
    const auto removed = sim::spatial_state::remove_unit(projected, spatial_, map_listeners_);
    end_spatial_change(true);
    if (removed != sim::spatial_state::Error::none) {
        fault_.note("death occupancy removal rejected spatial state");
        return;
    }
    if (auto* ground = ground_runtime(slot.unit_index))
        ground->project_slot();
    // Then, under line of sight, its own stamp leaves its owner's coverage.
    if ((state().game.visibility_flags & sim::visibility_state::update_sight_grid) != 0 &&
        sight_def != nullptr && sight_owner != nullptr) {
        auto context = sight_context();
        sim::visibility_state::clear_unit_sight(slot.record, *sight_def, *sight_owner, context);
    }
    release_target_observers(*slot.unit);
    const bool finished = std::bit_cast<uint32_t>(slot.record.build_remaining) == 0;
    if (outcome.killed_percent > 0 && finished)
        explode_unit(slot, death == DeathKind::self_destruct);
    if (outcome.wreck_level != 0) {
        spawn_corpse(slot, outcome.wreck_level, death != DeathKind::dismissed);
    }
    bridge_->runtime(slot).instance.reset();
    unit.object_present = false;
    unit.script_present = false;
    unit.flags = (unit.flags & ~live_unit_flag & ~death_pending_flag) & ~death_clear_mask;
    unit.record.type_index = 0;
    unit.health = 0;
    // The slot waits before a local creation takes it again (units.id-reuse-delay).
    sim::unit_spawn::record_slot_death(state(), state_.tables, slot.record);
    if (dummy_type_index < world_.types.size())
        unit.type = &world_.types[dummy_type_index].simulation;
    slot.movement_object = 0;
    if (auto* ground = movement_.at(slot.unit_index).get();
        ground != nullptr && !ground->mirrored_driver)
        sim::ground_orders::release_navigation(search_.controller, ground->navigation);
    movement_.at(slot.unit_index).reset();
    release_air_driver(slot.unit_index);
    for (auto& order : orders_)
        sim::air::air_goal_unlink(&order->air_goal, &slot.record);
    for (auto& goal : mirrored_air_goals_)
        sim::air::air_goal_unlink(&goal, &slot.record);
    for (auto& driver : air_drivers_)
        sim::air::air_goal_unlink(&driver.arrived, &slot.record);
    auto& player = world_.players.at(slot.record.owner_index);
    if (player.current_count && --player.current_count == 0 && last_unit.lost != nullptr)
        last_unit.lost(last_unit.context, slot.record.owner_index);
}

} // namespace oa::sim::match_runtime
