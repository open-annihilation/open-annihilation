// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/match_runtime.hpp"
#include "match_state.hpp"
#include "oa/sim/weapon_execution/interceptor.hpp"
#include "oa/sim/unit_health/paralysis.hpp"
#include "oa/sim/weapon_execution/retaliation.hpp"
#include "oa/sim/weapon_execution/weapon_keys.hpp"
#include <bit>
#include <cstdint>
#include "oa/sim/ballistics.hpp"
#include "oa/sim/ground_orders/orders.hpp"
#include "oa/sim/match_runtime/command.hpp"
#include <cmath>

namespace oa::sim::match_runtime {
namespace {
int16_t high_word(uint32_t value) {
    return std::bit_cast<int16_t>(static_cast<uint16_t>(value >> 16));
}

constexpr uint32_t fire_at_will = 2u << OA_UNIT_FLAG_FIRE_ORDER_SHIFT;
} // namespace

class TargetHost final : public sim::combat_state::IntelligenceHost,
                         public sim::combat_state::TargetSearchHost,
                         public sim::combat_state::AttackHost {
    Match& match;

    // An identity outside the pool is noted and names the reserved slot 0,
    // which holds no unit.
    sim::unit_spawn::Slot& slot(sim::combat_state::UnitIdentity id) {
        if (id == 0 || id >= match.slots_.size()) {
            match.fault_.note("target identity outside pool");
            return match.slots_[0];
        }
        return match.slots_[id];
    }

  public:

    explicit TargetHost(Match& world) : match(world) {}

    bool unit_active(sim::combat_state::UnitIdentity id) override {
        return unit_is_live_target(slot(id).unit->flags);
    }

    bool allied(uint8_t owner, uint8_t other) {
        if (owner >= 10 || other >= 10 || !match.player_alliances_[owner]) {
            match.fault_.note("target search requires resolved player alliances");
            return false;
        }
        return (*match.player_alliances_[owner])[other] != 0;
    }

    const sim::combat_state::TargetUnit* resolve(sim::combat_state::UnitIdentity id) override {
        auto& s = slot(id);
        auto& unit = *s.unit;
        if (!unit.type || !unit.owner)
            return nullptr;
        sim::simulation_state::Player* owner_view = unit.owner;
        auto& result = match.target_projection_[id];
        result = {
            id,
            reinterpret_cast<uintptr_t>(owner_view),
            unit.position,
            unit.flags,
            s.record.state_flags,
            unit.record.type_index,
            static_cast<uint8_t>(unit.type->flags >> 8)
        };
        return &result;
    }

    uint32_t random_bounded(uint32_t range) override { return match.random_.bounded(range); }

    void strategic_refresh(uint8_t player) {
        if (!match.strategic_environment_) {
            match.fault_.note(
                "strategic refresh requires resolved map and current wind environment"
            );
            return;
        }
        const auto environment = *match.strategic_environment_;
        auto& state = match.strategic_states_.at(player);
        std::vector<sim::combat_state::StrategicType> types(match.input_.types.size());
        state.owned_counts.assign(types.size(), 0);
        for (std::size_t id = 1; id < types.size(); ++id) {
            const auto& fields = match.input_.fields[id];
            if (!fields.definition || !fields.runtime_metadata) {
                match.fault_.note("strategic type requires resolved definition metadata");
                return;
            }
            const auto& definition = *fields.definition;
            auto& type = types[id];
            type.flags = match.input_.types[id].simulation.flags;
            type.abilities = (definition.can_attack ? sim::combat_state::ability_can_attack : 0u) |
                             (definition.can_load ? sim::combat_state::ability_can_load : 0u);
            type.makes_metal = static_cast<uint8_t>(definition.makes_metal);
            type.min_water_depth = fields.runtime_metadata->min_water_depth;
            type.radar_distance = definition.radar_distance;
            type.sonar_distance = definition.sonar_distance;
            type.build_cost_energy = static_cast<float>(definition.build_cost_energy);
            type.build_cost_metal = static_cast<float>(definition.build_cost_metal);
            type.energy_make = definition.energy_make;
            type.energy_use = definition.energy_use;
            type.extracts_metal = definition.extracts_metal;
            type.wind_generator = definition.wind_generator;
            type.tidal_generator = definition.tidal_generator;
            const auto binding = sim::combat_state::bind_unit_weapons(
                match.input_.weapons, {definition.weapon1, definition.weapon2, definition.weapon3}
            );
            for (std::size_t i = 0; i < 3; ++i) {
                const auto& weapon = *binding.weapons.definitions[i];
                type.weapons[i] = {
                    weapon.registry_index, weapon.default_damage, weapon.range_world_units
                };
            }
        }
        // The knowledge walk clears and recounts own completed live units before
        // the refresh draw. No unit-mutating callbacks intervene between that
        // scan and this callback.
        for (std::size_t id = 1; id < match.slots_.size(); ++id) {
            const auto& s = match.slots_[id];
            const auto& u = *s.unit;
            if (!u.owner || u.owner->record.index != player)
                continue;
            // Without the player's alliance row the refresh stops at its
            // first unit, before the strategic draw.
            if (player >= 10 || !match.player_alliances_[player]) {
                match.fault_.note("target search requires resolved player alliances");
                return;
            }
            if (!allied(player, player) || !unit_active(id))
                continue;
            const auto unfinished = s.record.build_remaining;
            if (unfinished != 0 && !std::isnan(unfinished))
                continue;
            auto& count = state.owned_counts.at(u.record.type_index);
            count =
                std::bit_cast<int16_t>(static_cast<uint16_t>(static_cast<uint16_t>(count) + 1u));
        }
        sim::combat_state::strategic_refresh(
            state,
            types,
            {match.world_.per_player_limit,
             match.world_.players[player].current_count,
             environment.maximum_wind,
             5000,
             environment.normalized_wind,
             environment.tidal_strength}
        );
    }

    int32_t search_radius(
        const sim::combat_state::TargetSource& source,
        const sim::combat_state::TargetSearchRequest& request
    ) override {
        if (request.explicit_radius) {
            const auto* weapon = match.weapons_[slot(source.identity).unit_index].definitions.at(
                request.weapon_slot
            );
            if (!weapon) {
                match.fault_.note("explicit target range requires initialized weapon definition");
                return 0;
            }
            return weapon->range_world_units;
        }
        return match.fields(slot(source.identity)).definition->sight_distance;
    }

    std::span<const sim::combat_state::TargetUnit>
    gather_nearby(uint8_t owner, const std::array<uint32_t, 3>& center, int32_t radius) override {
        if (owner >= match.sightings_.size()) {
            match.fault_.note("target partition outside player sightings");
            return {};
        }
        const auto& sightings = match.sightings_[owner];
        match.sighting_query_.clear();
        sim::combat_state::gather_sightings(
            {sightings.seen, sightings.seen_count},
            {sightings.radar, sightings.radar_count},
            sightings.radar_fallback != 0,
            center,
            radius,
            *this,
            match.sighting_query_
        );
        return match.sighting_query_;
    }

    bool global_target_override() const noexcept override {
        return (match.state().game.console_flags & OA_CONSOLE_FLAG_SHOOT_ALL) != 0;
    }

    uint8_t resolve_order(
        uint8_t requested,
        const sim::combat_state::AttackSource& source,
        const sim::combat_state::TargetUnit* target,
        const std::array<uint32_t, 3>*
    ) override {
        auto& from = slot(source.identity);
        const auto& definition = *match.fields(from).definition;
        const auto& weapons = match.weapons_[from.unit_index];
        CommandSource projected;
        projected.can_move = definition.can_move;
        projected.can_attack = definition.can_attack;
        // Unit.movement: only units with a movement runtime have the movement
        // object.
        projected.object_present = match.ground_runtime(from.unit_index) != nullptr;
        projected.unit_flags = from.unit->flags;
        projected.type_flags = from.unit->type->flags;
        projected.primary_weapon_flags = weapons.definitions[0]->flags;
        projected.secondary_weapon_flags = weapons.definitions[1]->flags;
        projected.type_primary_weapon_flags = weapons.definitions[0]->flags;
        projected.secondary_slot_flags = from.record.weapons[1].flags;
        projected.primary_surface_fire =
            match.rules_view()
                .weapon(oa::oa_weapon_def_id(&match.weapon_record(*weapons.definitions[0])))
                .surface_fire;
        const auto source_cap = pack_command_capabilities(
            definition.can_attack,
            definition.can_guard,
            definition.can_patrol,
            definition.can_move,
            definition.can_load,
            definition.can_reclamate,
            definition.can_resurrect,
            definition.can_capture,
            definition.can_fly,
            definition.is_airbase,
            definition.cant_be_transported
        );
        projected.abilities_byte0 = source_cap.abilities_byte0;
        projected.abilities_byte1 = source_cap.abilities_byte1;
        projected.flags_byte1 = source_cap.flags_byte1;
        projected.waterline = definition.waterline;
        projected.capacity = static_cast<uint8_t>(definition.transport_capacity);
        projected.size = static_cast<uint8_t>(definition.transport_size);
        projected.loaded_count = match.loaded_child_count(from.unit_index);
        std::optional<CommandTarget> other;
        if (target) {
            auto& to = slot(target->identity);
            const auto& to_def = *match.fields(to).definition;
            const auto& to_meta = *match.fields(to).runtime_metadata;
            const auto to_cap = pack_command_capabilities(
                to_def.can_attack,
                to_def.can_guard,
                to_def.can_patrol,
                to_def.can_move,
                to_def.can_load,
                to_def.can_reclamate,
                to_def.can_resurrect,
                to_def.can_capture,
                to_def.can_fly,
                to_def.is_airbase,
                to_def.cant_be_transported
            );
            CommandTarget cmd;
            cmd.allied = allied(from.unit->owner->record.index, to.unit->owner->record.index);
            cmd.unit_flags = to.unit->flags;
            cmd.type_flags = to.unit->type->flags;
            cmd.height = high_word(to.unit->position[1]);
            const auto to_top =
                static_cast<uint32_t>(match_unit_def(match, to.record).model_height);
            cmd.model_height = high_word(to_top);
            cmd.object_present = match.ground_runtime(to.unit_index) != nullptr;
            cmd.health = to.unit->health;
            cmd.max_health = to_def.max_damage;
            cmd.occupancy = static_cast<uint8_t>(to.unit->flags & 3u);
            cmd.flags_byte1 = to_cap.flags_byte1;
            cmd.abilities_byte2 = to_cap.abilities_byte2;
            cmd.footprint = to_meta.footprint_x;
            cmd.waterline = to_meta.min_water_depth;
            cmd.y = to.unit->position[1];
            cmd.y_offset = to_top;
            cmd.progress = to.record.build_remaining;
            other = cmd;
        }
        const auto name =
            resolve_combat_command(requested, projected, other, match.simulation_.sea_level);
        if (name.empty())
            return 0;
        const auto kind = combat_order_kind(name);
        if (kind == 0)
            match.fault_.note("resolved mission handler not integrated", name);
        return kind;
    }

    bool commit_orders(
        const sim::combat_state::AttackSource& source,
        std::span<const sim::combat_state::AttackOrderRequest> requests
    ) override {
        return match.commit_attack_orders(source, requests);
    }

    bool weapon_can_reach(
        const sim::combat_state::TargetSource& source,
        const sim::combat_state::TargetUnit& target,
        uint8_t weapon_slot
    ) override {
        auto& from = slot(source.identity);
        auto& to = slot(target.identity);
        const auto* weapon = match.weapons_[from.unit_index].definitions.at(weapon_slot);
        if (!weapon) {
            match.fault_.note("target range requires initialized weapon definition");
            return false;
        }
        const auto rules = match.rules_view();
        const auto& record = match.weapon_record(*weapon);
        const sim::ballistics::WeaponReachParameters reach{
            weapon->flags,
            weapon->range_world_units,
            sim::weapon_execution::ballistic_parameters(
                weapon->projectile_velocity,
                weapon->minimum_barrel_angle_radians,
                match.state().game.gravity,
                rules.rules()
            ),
            sim::weapon_execution::reach_keys(rules.weapon(oa::oa_weapon_def_id(&record)))
        };
        return sim::ballistics::weapon_can_reach(
            reach, reach_geometry(from), reach_geometry(to), match.simulation_.sea_level
        );
    }

  private:

    /// Returns what the reach test reads of a unit: its position, the whole
    /// world units of its type's model top, its flags and its type's flags.
    ///
    /// @param unit unit slot
    /// @return the unit's reach geometry
    sim::ballistics::ReachUnitGeometry reach_geometry(const sim::unit_spawn::Slot& unit) {
        const auto height = match_unit_def(match, unit.record).model_height;
        return {
            unit.unit->position,
            high_word(static_cast<uint32_t>(height)),
            unit.unit->flags,
            unit.unit->type->flags
        };
    }
};

sim::simulation_state::Unit* Match::find_automatic_target(sim::simulation_state::Unit& unit) {
    if ((unit.record.flags & OA_UNIT_FLAG_FIRE_ORDER_MASK) != fire_at_will)
        return nullptr;
    return search_automatic_target(unit, {0, false});
}

sim::simulation_state::Unit* Match::search_automatic_target(
    sim::simulation_state::Unit& unit, const sim::combat_state::TargetSearchRequest& request
) {
    sim::unit_spawn::Slot* slot = nullptr;
    for (auto& candidate : slots_)
        if (candidate.unit == &unit) {
            slot = &candidate;
            break;
        }
    if (!slot) {
        fault_.note("target source outside pool");
        return nullptr;
    }
    const auto* masks = fields(*slot).target_masks;
    if (!masks) {
        fault_.note("automatic targeting requires resolved category masks");
        return nullptr;
    }
    TargetHost host(*this);
    const auto* resolved = host.resolve(slot->unit_index);
    if (!resolved) {
        fault_.note("target source has no type or owner");
        return nullptr;
    }
    sim::combat_state::TargetSource source;
    static_cast<sim::combat_state::TargetUnit&>(source) = *resolved;
    source.owner_spatial_index = slot->record.owner_index;
    source.owner_present = unit.owner && unit.owner->present;
    source.owner_status = unit.owner && unit.owner->present ? unit.owner->status : 0;
    source.type_flags = unit.type->flags;
    source.preferred_category_masks = {
        masks->primary_bad.words, masks->secondary_bad.words, masks->special_bad.words
    };
    source.implicit_excluded_category_mask = masks->no_chase.words;
    for (std::size_t i = 0; i < source.weapon_flags.size(); ++i) {
        const auto* definition = weapons_[slot->unit_index].definitions[i];
        if (!definition) {
            fault_.note("target source weapon not initialized");
            return nullptr;
        }
        source.weapon_flags[i] = static_cast<uint8_t>(definition->flags);
    }
    const auto result = sim::combat_state::select_automatic_target(source, request, host);
    return result ? slots_.at(result).unit : nullptr;
}

bool Match::weapon_can_reach(uint16_t source, uint16_t target, uint8_t weapon) {
    TargetHost host(*this);
    const auto* resolved_source = host.resolve(source);
    const auto* resolved_target = host.resolve(target);
    if (!resolved_source || !resolved_target)
        return false;
    sim::combat_state::TargetSource from;
    static_cast<sim::combat_state::TargetUnit&>(from) = *resolved_source;
    return host.weapon_can_reach(from, *resolved_target, weapon);
}

bool Match::issue_automatic_attack(
    sim::simulation_state::Unit& source, sim::simulation_state::Unit& target
) {
    sim::unit_spawn::Slot* from = nullptr;
    sim::unit_spawn::Slot* to = nullptr;
    for (auto& slot : slots_) {
        if (slot.unit == &source)
            from = &slot;
        if (slot.unit == &target)
            to = &slot;
    }
    if (!from || !to) {
        fault_.note("attack identities outside match pool");
        return false;
    }
    return issue_attack(from->unit_index, to->unit_index, false);
}

bool Match::issue_attack(uint16_t source_index, uint16_t target_index, bool forced) {
    auto& from = slots_.at(source_index);
    auto& source = *from.unit;
    TargetHost host(*this);
    const auto* target = host.resolve(target_index);
    if (!target)
        return false;
    sim::combat_state::AttackSource request{
        source_index,
        source.flags,
        source.position,
        static_cast<uint16_t>(fields(from).definition->maneuver_leash_length)
    };
    return sim::combat_state::issue_attack_order(request, *target, forced, host);
}

bool Match::issue_attack_command(
    uint16_t source_index, uint16_t target_index, bool queue, const sim::ground_orders::Point* point
) {
    if (source_index == target_index)
        return false;
    auto& from = slots_.at(source_index);
    TargetHost host(*this);
    const auto* aimed = host.resolve(target_index);
    if (aimed == nullptr)
        return false;
    // The command itself carries no leash: that belongs to the attacks a
    // unit starts on its own.
    const sim::combat_state::AttackSource request{
        source_index, from.unit->flags, from.unit->position, 0
    };
    const auto kind =
        host.resolve_order(sim::combat_state::attack_order_kind, request, aimed, nullptr);
    if (kind == 0)
        return false;
    // The order keeps the target only when its kind takes one: a Suppress
    // order on an allied unit fires at the point alone.
    return !order_refused(issue_order(source_index, kind, queue, target_index, point, 0, 0));
}

sim::simulation_state::Order* Match::issue_attack_ground(
    uint16_t source_index, const sim::ground_orders::Point& destination, bool queue
) {
    auto& from = slots_.at(source_index);
    TargetHost host(*this);
    const sim::combat_state::AttackSource request{
        source_index, from.unit->flags, from.unit->position, 0
    };
    const auto kind =
        host.resolve_order(sim::combat_state::attack_order_kind, request, nullptr, nullptr);
    if (kind == 0)
        return nullptr;
    auto& order = issue_order(source_index, kind, queue, 0, &destination, 0, 0);
    return order_refused(order) ? nullptr : &order;
}

void Match::retarget_weapon_slot(sim::unit_spawn::Slot& unit, uint8_t slot) {
    auto& world = state();
    const auto* weapon = oa::world_weapon_def(&world, unit.record.weapons[slot].def);
    if (weapon != nullptr && (weapon->flags & OA_WEAPON_FLAG_INTERCEPTOR) != 0) {
        const auto ref = sim::weapon_execution::find_interceptor_target(world, unit.record, slot);
        if (const auto* shot = oa::world_projectile(&world, ref)) {
            sim::weapon_execution::aim_slot_at_point(unit.record, shot->position, slot);
            return;
        }
    } else {
        if ((unit.record.flags & OA_UNIT_FLAG_FIRE_ORDER_MASK) != fire_at_will)
            return;
        if (const auto* target = search_automatic_target(*unit.unit, {slot, true})) {
            sim::weapon_execution::aim_slot_at_unit(unit.record, target->record, slot);
            return;
        }
    }
    stop_weapon(unit, slot);
}

void Match::sweep_weapon_targets(uint8_t player, bool computer) {
    auto& world = state();
    const auto* owner_record = oa::world_player(&world, player);
    if (owner_record == nullptr)
        return;
    const auto& owner = *owner_record;
    uint32_t count = 0;
    const auto* first = oa::world_player_units(&world, &owner, &count);
    if (first == nullptr)
        return;
    const auto first_slot = static_cast<uint16_t>(oa::world_unit_slot(&world, first));
    const auto last_slot = static_cast<uint16_t>(first_slot + count - 1u);
    auto& cursor = weapon_sweep_cursor_[player];
    // weapons.retarget-out-of-range: a slot's unit target out of its reach is
    // dropped, and the pass also takes units whose fire order is 3.
    const bool drop_out_of_range = rules().weapons.retarget_out_of_range.enabled;
    for (uint32_t step = 0; step <= world.game.units_per_player / 30u; ++step) {
        cursor =
            cursor == 0 || cursor == last_slot ? first_slot : static_cast<uint16_t>(cursor + 1u);
        if (cursor >= slots_.size())
            continue;
        auto& unit = slots_[cursor];
        const auto& record = unit.record;
        const auto fire_order = record.flags & OA_UNIT_FLAG_FIRE_ORDER_MASK;
        if (record.type_index == 0 || record.build_remaining != 0.0F ||
            (record.flags & OA_UNIT_FLAG_HAS_WEAPONS) == 0 ||
            (drop_out_of_range ? (fire_order & fire_at_will) == 0 : fire_order != fire_at_will))
            continue;
        const auto& aims = record.weapons;
        for (uint8_t slot = 0; slot < std::size(aims); ++slot) {
            const auto* weapon = oa::world_weapon_def(&world, record.weapons[slot].def);
            if ((aims[slot].flags & OA_UNIT_WEAPON_ENABLED) == 0 ||
                (aims[slot].flags & OA_UNIT_WEAPON_RETALIATE) == 0 || weapon == nullptr ||
                (weapon->flags & OA_WEAPON_FLAG_DROPPED) != 0 ||
                (!computer && (weapon->flags & OA_WEAPON_FLAG_COMMAND_FIRE) != 0))
                continue;
            // The slot's unit target stays while it is an enemy outside
            // the slot's bad-target category that a paralyzer has not already
            // stunned. A dead target stays too; the weapon tick drops it.
            if (aims[slot].target_b == OA_UNIT_TARGET_IS_UNIT && aims[slot].target_a > 0 &&
                static_cast<std::size_t>(aims[slot].target_a) < slots_.size()) {
                const auto target_index = static_cast<uint16_t>(aims[slot].target_a);
                // An out-of-reach target loses only the slot's target index:
                // the slot is left without a target and no TargetCleared
                // runs. The tests below still judge the old target, which
                // keeps the slot empty until the pass comes round again.
                if (drop_out_of_range && weapons_[cursor].definitions[slot] != nullptr &&
                    !weapon_can_reach(cursor, target_index, slot))
                    unit.record.weapons[slot].target_a = 0;
                const auto& target = slots_[target_index].record;
                const auto* target_owner = oa::world_unit_owner(&world, &target);
                const auto* masks = fields(unit).target_masks;
                const auto type = static_cast<uint16_t>(target.type_index);
                const bool bad_target =
                    masks != nullptr && (slot == 0   ? masks->primary_bad.contains(type)
                                         : slot == 1 ? masks->secondary_bad.contains(type)
                                                     : masks->special_bad.contains(type));
                const bool stunned =
                    (weapon->flags & OA_WEAPON_FLAG_PARALYZER) != 0 &&
                    (target.state_flags & sim::unit_health::paralyzed_state_flag) != 0;
                if ((target_owner == nullptr || !allied(player, target_owner->index)) &&
                    !bad_target && !stunned)
                    continue;
            }
            retarget_weapon_slot(unit, slot);
        }
    }
}

void Match::refresh_strategic_state(uint8_t player) {
    TargetHost host(*this);
    host.strategic_refresh(player);
}

} // namespace oa::sim::match_runtime
