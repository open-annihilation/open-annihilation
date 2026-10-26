// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "tick_internal.hpp"

#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <type_traits>

namespace oa::sim::match_runtime {
namespace {
// The wreck level is the low four bits of the Killed script's second result.
constexpr int32_t killed_wreck_level_mask = 0xf;

// The heal remainders are digested and saved as their bytes: two 32-bit
// words per entry, with no padding.
static_assert(
    sizeof(sim::unit_health::RepairRemainders) ==
    sim::unit_health::repair_remainder_targets * 2 * sizeof(uint32_t)
);
static_assert(std::has_unique_object_representations_v<sim::unit_health::RepairRemainders>);

/// Returns the bytes of a match's heal remainders.
///
/// @param context the match
/// @return the table's bytes
std::span<const uint8_t> repair_remainder_bytes(void* context) {
    const auto table = static_cast<Match*>(context)->repair_remainders();
    return {reinterpret_cast<const uint8_t*>(table.data()), table.size_bytes()};
}

/// Replaces a match's heal remainders with saved bytes.
///
/// @param context the match
/// @param bytes the saved table
/// @return false when they are not a table of the match's size
bool restore_repair_remainders(void* context, std::span<const uint8_t> bytes) {
    const auto table = static_cast<Match*>(context)->repair_remainders();
    if (bytes.size() != table.size_bytes())
        return false;
    if (!bytes.empty())
        std::memcpy(table.data(), bytes.data(), bytes.size());
    return true;
}
} // namespace

void Match::keep_repair_remainders() {
    if (input_.rules.repair.rate.mode != data::match_rules::RepairRateMode::exact_remainder)
        return;
    repair_remainder_count_ = slots_.size();
    repair_remainders_ =
        std::make_unique<sim::unit_health::RepairRemainders[]>(repair_remainder_count_);
    RuleStateTable table{};
    table.name = "repair-remainders";
    table.context = this;
    table.bytes = repair_remainder_bytes;
    table.restore = restore_repair_remainders;
    if (!add_rule_state(rule_state_, table))
        fault_.note("repair remainders do not fit the rule state");
}

sim::unit_spawn::Slot& TickHost::HealthHost::slot(const sim::unit_health::Unit& u) {
    return match_.world().slots[u.identity];
}

void TickHost::HealthHost::write_stand_in(const sim::unit_health::Unit& u) {
    if (&u != stand_in_)
        return;
    auto& s = slot(u);
    s.unit->health = u.health;
    s.unit->flags = u.flags;
    s.unit->events = u.events;
    s.record.build_remaining = u.build_remaining;
}

void TickHost::HealthHost::read_stand_in(sim::unit_health::Unit& u) {
    if (&u != stand_in_)
        return;
    const auto& s = slot(u);
    u.health = s.unit->health;
    u.flags = s.unit->flags;
    u.events = s.unit->events;
    u.build_remaining = s.record.build_remaining;
}

void TickHost::HealthHost::store() {
    economy_.energy.requested = debit_.requested;
    economy_.energy.accepted = debit_.accepted;
    economy_.energy.gate = debit_.gate;
    economy_.metal.requested = metal_.requested;
    economy_.metal.accepted = metal_.accepted;
    economy_.metal.gate = metal_.gate;
}

sim::unit_health::EconomyDebit& TickHost::HealthHost::energy_debit(sim::unit_health::Unit&) {
    return debit_;
}

sim::unit_health::EconomyDebit& TickHost::HealthHost::metal_debit(sim::unit_health::Unit&) {
    return metal_;
}

void TickHost::HealthHost::refund_metal(sim::unit_health::Unit& target, float amount) {
    match_.credit_metal(slot(target).record, amount);
}

void TickHost::HealthHost::complete_construction(
    sim::unit_health::Unit& builder, sim::unit_health::Unit& target
) {
    write_stand_in(target);
    match_.link_built_unit(slot(target), slot(builder));
    read_stand_in(target);
}

void Match::link_built_unit(sim::unit_spawn::Slot& unit, sim::unit_spawn::Slot& builder) {
    unit.record.build_remaining = 0.0F;
    unit.unit->flags |= sim::unit_health::construction_dirty_flag;
    // Only the machine that simulates the unit sets it down and shares the
    // completion; the other players take the carry link it shares.
    const bool simulated_here = sim::simulation_state::locally_simulated(*unit.unit);
    // Mobile (not bm_code==0) nanoframes detach from the pad first and get no
    // order here; their GetBuilt takes them off it.
    if (simulated_here && !(unit.unit->flags & building_unit_flag) && unit.record.attach_parent)
        set_carry_link(unit.unit_index, 0, -1, 1);
    const auto& definition = *fields(unit).definition;
    if (definition.activate_when_built)
        set_activation(unit, 1, true);
    // A finished wall stops being a unit. It is marked to die at once,
    // dismissed, so the feature named by its corpse takes the same footprint
    // at that feature's own health, in the builder's colour. Every machine
    // marks it, as one marks a wall placed already finished. The builder's
    // order still finishes, because the wall is removed on its own turn,
    // after the order has said the build is complete.
    if (definition.is_feature) {
        unit.record.flags |= death_pending_flag;
        unit.record.damage_kind = static_cast<uint8_t>(DeathKind::dismissed);
    }
    unit.unit->events =
        static_cast<uint16_t>(unit.unit->events | sim::unit_health::construction_event);
    if (simulated_here)
        notify_finished(unit, builder);
    // Every build that reaches the end passes here.
    report_finished(unit.unit_index, builder.unit_index);
}

void Match::finish_unit(uint16_t unit, uint16_t builder) {
    auto& finished = slots_.at(unit);
    if (builder >= slots_.size())
        return;
    auto& source = slots_[builder];
    if (!finished.unit || !source.unit || !(finished.record.flags & live_unit_flag) ||
        !(source.record.flags & live_unit_flag))
        return;
    link_built_unit(finished, source);
}

bool TickHost::HealthHost::target_is_live(const sim::unit_health::Unit& u) {
    return unit_is_live_target(slot(u).unit->flags);
}

void TickHost::HealthHost::apply_health_event(
    sim::unit_health::Unit& target,
    const sim::unit_health::Unit* source,
    const sim::unit_health::HealthEvent& event
) {
    write_stand_in(target);
    match_.apply_damage_event(
        this->slot(target),
        source ? &this->slot(*source) : nullptr,
        event.amount,
        event.kind,
        event.direction
    );
    read_stand_in(target);
}

bool TickHost::HealthHost::target_owner_present(const sim::unit_health::Unit& u) {
    return slot(u).unit->owner && slot(u).unit->owner->present;
}

uint8_t TickHost::HealthHost::target_owner_status(const sim::unit_health::Unit& u) {
    return slot(u).unit->owner ? slot(u).unit->owner->status : 0;
}

sim::unit_health::RouteIdentity
TickHost::HealthHost::source_owner_route(const sim::unit_health::Unit& u) {
    const auto& multiplayer = match_.multiplayer;
    if (multiplayer.health_route)
        return multiplayer.health_route(multiplayer.context, u.identity);
    match_.fault_.note("health event route without a multiplayer handler");
    return {};
}

sim::unit_health::RouteIdentity TickHost::HealthHost::fallback_route() {
    const auto& multiplayer = match_.multiplayer;
    if (multiplayer.health_route)
        return multiplayer.health_route(multiplayer.context, 0);
    match_.fault_.note("health event fallback route without a multiplayer handler");
    return {};
}

void TickHost::HealthHost::share_health_event(
    sim::unit_health::RouteIdentity route, const sim::unit_health::HealthEvent& event
) {
    const auto& multiplayer = match_.multiplayer;
    if (multiplayer.health_shared) {
        multiplayer.health_shared(multiplayer.context, route, event);
        return;
    }
    match_.fault_.note("health event shared without a multiplayer handler");
}

bool TickHost::nano_repair(
    sim::unit_spawn::Slot& repairer_slot, sim::simulation_state::Unit& patient
) {
    auto& patient_slot = slot(patient);
    const auto* definition = match.fields(repairer_slot).definition;
    const auto* patient_definition = match.fields(patient_slot).definition;
    if (!definition || !patient_definition || !patient.type || !repairer_slot.unit ||
        !repairer_slot.unit->type) {
        match.fault_.note("natural repair is missing a definition");
        return false;
    }
    auto make_type = [](const auto& fields, const sim::simulation_state::Unit& unit) {
        return sim::unit_health::UnitType{
            fields.damage_modifier_fixed,
            static_cast<float>(fields.build_cost_energy),
            fields.build_time,
            unit.type->maximum_health,
            static_cast<float>(fields.build_cost_metal)
        };
    };
    auto healer_type = make_type(*definition, *repairer_slot.unit);
    auto patient_type = make_type(*patient_definition, patient);
    sim::unit_health::Unit repairer{
        repairer_slot.unit_index,
        repairer_slot.record.state_flags,
        repairer_slot.record.veteran_level,
        repairer_slot.unit->health,
        &healer_type
    };
    sim::unit_health::Unit projected{
        patient_slot.unit_index,
        patient_slot.record.state_flags,
        patient_slot.record.veteran_level,
        patient.health,
        &patient_type
    };
    projected.type_index = patient_slot.record.type_index;
    HealthHost health(match, patient_slot.unit_index);
    // Every repair order passes the repairer's worker time per tick.
    const auto rate = static_cast<float>(
        static_cast<uint32_t>(static_cast<uint16_t>(definition->worker_time)) / 30u
    );
    const auto repair = sim::unit_health::recover_health(repairer, projected, rate, health);
    health.store();
    patient.health = projected.health;
    if (repair.performed)
        match.spray_nano(repairer_slot, patient, Match::NanoSpray::repair);
    return repair.performed;
}

void TickHost::scaled_damage(
    sim::unit_spawn::Slot* source, sim::unit_spawn::Slot& target, int32_t amount, uint32_t kind
) {
    auto& u = *target.unit;
    const auto& definition = *match.fields(target).definition;
    sim::unit_health::UnitType type{
        definition.damage_modifier_fixed,
        static_cast<float>(definition.build_cost_energy),
        definition.build_time,
        u.type->maximum_health,
        static_cast<float>(definition.build_cost_metal)
    };
    sim::unit_health::Unit projected{
        target.unit_index, target.record.state_flags, target.record.veteran_level, u.health, &type
    };
    projected.type_index = target.record.type_index;
    sim::unit_health::Unit attacker{};
    if (source)
        attacker = {
            source->unit_index,
            source->record.state_flags,
            source->record.veteran_level,
            source->unit->health,
            nullptr
        };
    HealthHost host(match, target.unit_index);
    sim::unit_health::submit_damage(source ? &attacker : nullptr, projected, amount, kind, host, 0);
    host.store();
    if (kind == healing_kind)
        u.health = projected.health;
}

void TickHost::apply_scaled_damage(oa::Unit& record, int32_t amount, uint32_t kind) {
    scaled_damage(nullptr, slot(unit_view(record)), amount, kind);
}

void TickHost::regenerate_health(oa::Unit& record) {
    auto& u = unit_view(record);
    auto& s = slot(u);
    const auto& definition = *match.fields(s).definition;
    sim::unit_health::UnitType type{
        definition.damage_modifier_fixed,
        static_cast<float>(definition.build_cost_energy),
        definition.build_time,
        u.type->maximum_health,
        static_cast<float>(definition.build_cost_metal)
    };
    sim::unit_health::Unit projected{
        s.unit_index, s.record.state_flags, s.record.veteran_level, u.health, &type
    };
    projected.type_index = s.record.type_index;
    HealthHost host(match, s.unit_index);
    const auto rate = sim::simulation_state::self_heal_rate(
        match.rules().repair.healtime_self_heal, match_unit_def(match, s.record).heal_time
    );
    (void)sim::unit_health::recover_health(projected, projected, rate, host);
    host.store();
    u.health = projected.health;
}

void TickHost::kill_unit(oa::Unit& record, uint8_t kind) {
    auto& u = unit_view(record);
    auto& s = slot(u);
    if (!(u.flags & live_unit_flag))
        return;
    const bool commander = side_commander(match.state(), s.record);
    if (commander)
        match.world_.players.at(s.record.owner_index).resource_flags &=
            static_cast<uint8_t>(~OA_PLAYER_RESOURCE_SHARES_STORAGE);
    int32_t killed_percent = 0;
    int32_t killed_flag = 0;
    if (kind == static_cast<uint8_t>(DeathKind::dismissed)) {
        killed_percent = 0;
        killed_flag = 1;
    } else if (
        kind == static_cast<uint8_t>(DeathKind::captured) ||
        kind == static_cast<uint8_t>(DeathKind::reclaim) ||
        kind == static_cast<uint8_t>(DeathKind::cancelled) || u.health > 0
    ) {
        killed_percent = 0;
        killed_flag = 0;
    } else {
        // A type with no maximum health takes a quotient of 0.
        const auto maximum = u.type->maximum_health;
        const auto quotient =
            maximum != 0 ? (static_cast<int32_t>(u.health) * -100) / static_cast<int32_t>(maximum)
                         : 0;
        killed_percent =
            static_cast<int32_t>(quotient + static_cast<int32_t>(u.previous_health_percent)) / 2;
        if (killed_percent < 1)
            killed_percent = 1;
        if (killed_percent > 100)
            killed_percent = 100;
        if (auto* instance = match.instance(s.unit_index); instance && instance->script()) {
            std::array<int32_t, 4> args{killed_percent, killed_flag, 0, 0};
            instance->script()->query(ScriptFunction::killed, args);
            killed_percent = args[0];
            killed_flag = args[1];
        }
    }
    // Unfinished units leave no wreck.
    if (std::bit_cast<uint32_t>(s.record.build_remaining) != 0)
        killed_flag = 0;
    // A last attacker whose player's slot has gone free credits nobody, here
    // and on the other players' machines, as in 3.1c.
    if (const auto* attacker_owner = world_player(&match.state(), s.record.last_attacker_owner);
        attacker_owner != nullptr && attacker_owner->status == OA_PLAYER_STATUS_FREE)
        s.record.last_attacker_owner = OA_PLAYER_COUNT;
    const KillOutcome outcome{
        static_cast<DeathKind>(kind),
        static_cast<int8_t>(killed_percent),
        static_cast<uint8_t>(killed_flag & killed_wreck_level_mask)
    };
    const bool local = sim::simulation_state::locally_simulated(u);
    // The machine simulating the unit shares its death before it tears the
    // unit down, while the record still names its owner and last attacker.
    if (local && match.multiplayer.unit_killed != nullptr)
        match.multiplayer.unit_killed(match.multiplayer.context, s.unit_index, outcome);
    match.note_step(sim::simulation_state::clear_orders(u, true, *this));
    const auto owner = s.record.owner_index;
    match.teardown_dead_unit(s, outcome);
    // Only the machine simulating the commander runs its player's sweep,
    // after the order panel goes back to its root page.
    if (commander && match.state().game.session_rules != 0 && local) {
        if (match.order_panel.close != nullptr)
            match.order_panel.close(match.order_panel.context);
        destroy_player_units(owner);
    }
}

void TickHost::destroy_player_units(uint8_t owner) {
    auto& world = match.state();
    const auto& player = match_player(match, owner);
    if (player.unit_count == 0)
        return;
    uint32_t count = 0;
    auto* first = oa::world_player_units(&world, &player, &count);
    for (uint32_t i = 0; i < count; ++i) {
        auto& s = match.slots_.at(oa::world_unit_slot(&world, first + i));
        if (!s.unit || !(s.unit->flags & live_unit_flag) || (s.unit->flags & death_pending_flag))
            continue;
        const auto self_destruct = static_cast<uint8_t>(DeathKind::self_destruct);
        if (sim::simulation_state::locally_simulated(*s.unit)) {
            scaled_damage(&s, s, self_destruct_damage, self_destruct);
            continue;
        }
        // A unit simulated elsewhere blows up and dies here at once.
        match.explode_unit(s, true);
        s.unit->flags |= death_pending_flag;
        kill_unit(s.record, self_destruct);
    }
}

void Match::kill_unit(uint16_t unit, uint8_t kind) {
    TickHost(*this).kill_unit(slots_.at(unit).record, kind);
}

void Match::apply_kill(
    uint16_t unit, const KillOutcome& outcome, uint16_t attacker, uint8_t attacker_owner
) {
    auto& s = slots_.at(unit);
    if (s.unit == nullptr || !(s.unit->flags & live_unit_flag))
        return;
    s.record.last_attacker_id = attacker;
    s.record.last_attacker_owner = attacker_owner;
    TickHost host(*this);
    note_step(sim::simulation_state::clear_orders(*s.unit, true, host));
    teardown_dead_unit(s, outcome, true);
}

} // namespace oa::sim::match_runtime
