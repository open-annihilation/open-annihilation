// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "tick_internal.hpp"
#include "oa/sim/weapon_execution/interceptor.hpp"
#include "oa/sim/weapon_execution/projectile_pool.hpp"
#include "oa/sim/weapon_execution/weapon_keys.hpp"

#include <cstdint>
#include <utility>

namespace oa::sim::match_runtime {

class WeaponTickHost final : public sim::weapon_execution::Host {
    Match& match;
    sim::unit_spawn::Slot& source;

    ScriptInstance* script() {
        auto* instance = match.instance(source.unit_index);
        return instance ? instance->script() : nullptr;
    }

    uint16_t heading() {
        if (auto* g = match.ground_runtime(source.unit_index))
            return g->geometry.heading;
        return source.yaw;
    }

  public:

    WeaponTickHost(Match& world, sim::unit_spawn::Slot& slot) : match(world), source(slot) {
        rules = world.rules_view();
    }

    bool resolve_target(uint8_t index, sim::weapon_execution::Target& out) override {
        const auto& weapon_slot = source.record.weapons[index];
        if (weapon_slot.target_b == OA_UNIT_TARGET_IS_UNIT) {
            if (!weapon_slot.target_a)
                return false;
            const auto target_index = static_cast<uint16_t>(weapon_slot.target_a);
            if (target_index >= match.slots_.size()) {
                match.fault_.note("weapon target outside pool");
                return false;
            }
            auto& target = match.slots_[target_index];
            if (!target.unit->record.type_index) {
                match.stop_weapon(source, index);
                return false;
            }
            auto* instance = match.instance(target.unit_index);
            auto point = instance ? instance->sweet_spot_world() : target.unit->position;
            // Veterans lead a target that has a movement object.
            const auto* definition = match.weapons_[source.unit_index].definitions[index];
            const auto* mover = match.ground_runtime(target.unit_index);
            if (definition) {
                const auto& weapon = match.state().game.weapon_defs[definition->registry_index];
                if (sim::weapon_execution::lead_applies(
                        weapon, source.record, mover != nullptr, rules
                    )) {
                    const auto lead = sim::weapon_execution::veteran_lead_offset(
                        weapon,
                        source.record,
                        {std::bit_cast<int32_t>(point[0]),
                         std::bit_cast<int32_t>(point[1]),
                         std::bit_cast<int32_t>(point[2])},
                        {mover->movement.velocity[0],
                         mover->movement.velocity[1],
                         mover->movement.velocity[2]}
                    );
                    for (size_t axis = 0; axis < 3; ++axis)
                        point[axis] += std::bit_cast<uint32_t>(lead[axis]);
                }
            }
            out.point = as_point(point);
            out.unit_identity = target_index;
            return true;
        }
        const auto x = static_cast<int32_t>(weapon_slot.target_a) << 16;
        const auto z = static_cast<int32_t>(weapon_slot.target_b) << 16;
        auto height = static_cast<int32_t>(match.simulation_.sea_level);
        const auto terrain =
            match.sample_terrain_height(static_cast<uint32_t>(x), static_cast<uint32_t>(z));
        if (height < terrain)
            height = terrain;
        out.point.fixed = {
            static_cast<uint32_t>(x), static_cast<uint32_t>(height) << 16, static_cast<uint32_t>(z)
        };
        out.unit_identity = 0;
        return true;
    }

    sim::weapon_execution::Point source_position() override {
        return as_point(source.unit->position);
    }

    struct AimAngles {
        int16_t yaw{};
        int16_t pitch{};
    };

    // The turret angles the slot's weapon wants for the target, from the
    // AimFrom piece; none when the solver finds no pitch.
    std::optional<AimAngles>
    desired_aim(uint8_t index, const sim::weapon_execution::Target& target) {
        const auto* definition = match.weapons_[source.unit_index].definitions[index];
        if (!definition)
            return std::nullopt;
        auto* instance = match.instance(source.unit_index);
        if (!instance)
            return std::nullopt;
        const auto aim = instance->aim_from_world(index);
        const auto goal = as_position(target.point);
        const auto solution = sim::ballistics::turret_aim(
            definition->flags,
            sim::weapon_execution::ballistic_parameters(
                definition->projectile_velocity,
                definition->minimum_barrel_angle_radians,
                match.state().game.gravity,
                rules.rules()
            ),
            {{signed_word(aim[0]), signed_word(aim[1]), signed_word(aim[2])},
             {signed_word(goal[0]), signed_word(goal[1]), signed_word(goal[2])},
             static_cast<int16_t>(heading())}
        );
        if (!solution.feasible)
            return std::nullopt;
        return AimAngles{solution.heading, solution.pitch};
    }

    // Clears aim_ready and starts the slot's Aim script with the slot's
    // return callback; the angles go in as unsigned 16-bit values. The other
    // players start it with the same two arguments.
    void start_aim_script(uint8_t index, uint16_t heading, uint16_t pitch) {
        auto& weapon_slot = source.record.weapons[index];
        weapon_slot.aim_ready = 0;
        const std::array<int32_t, 2> args{heading, pitch};
        if (auto* cob = script())
            cob->call(aim_script_names[index], args, false, [&weapon_slot](int32_t result) {
                sim::weapon_execution::record_aim_result(weapon_slot, result);
            });
        match.share_named_script_start(
            source.unit_index,
            aim_script_names[index],
            static_cast<uint8_t>(args.size()),
            {heading, pitch, 0, 0}
        );
    }

    /// Aims a turret slot at the target and starts its Aim script with the
    /// angles.
    ///
    /// tick_weapons tests and sets the slot's aimed bit around it.
    ///
    /// @param index Weapon slot 0..2.
    /// @param target Resolved target point and unit.
    /// @return False when the aim solver finds no angles.
    bool begin_turret_aim(uint8_t index, const sim::weapon_execution::Target& target) override {
        const auto angles = desired_aim(index, target);
        if (!angles)
            return false;
        auto& weapon_slot = source.record.weapons[index];
        weapon_slot.aim_heading = angles->yaw;
        weapon_slot.aim_pitch = angles->pitch;
        start_aim_script(
            index, static_cast<uint16_t>(angles->yaw), static_cast<uint16_t>(angles->pitch)
        );
        return true;
    }

    sim::weapon_execution::TurretAim
    turret_aim(uint8_t index, const sim::weapon_execution::Target& target) override {
        const auto* definition = match.weapons_[source.unit_index].definitions[index];
        const auto desired = desired_aim(index, target);
        if (!definition || !desired || !source.unit)
            return sim::weapon_execution::TurretAim::unsolved;
        const auto& weapon_slot = source.record.weapons[index];
        const bool within = sim::weapon_execution::turret_within_tolerance(
            {definition->tolerance,
             definition->pitch_tolerance,
             weapon_slot.aim_heading,
             weapon_slot.aim_pitch,
             desired->yaw,
             desired->pitch,
             (source.record.flags & OA_UNIT_FLAG_MOVE_RATE_MASK) != 0}
        );
        return within ? sim::weapon_execution::TurretAim::on_target
                      : sim::weapon_execution::TurretAim::off_target;
    }

    /// Starts a vertical-launch slot's Aim script with zero angles.
    ///
    /// tick_weapons tests and sets the slot's aimed bit around it.
    ///
    /// @param index Weapon slot 0..2.
    void begin_vlaunch_aim(uint8_t index) override { start_aim_script(index, 0, 0); }

    bool aim_ready(uint8_t index) override { return source.record.weapons[index].aim_ready != 0; }

    bool can_reach(
        uint8_t index,
        const sim::weapon_execution::Point& from,
        const sim::weapon_execution::Target& target
    ) override {
        const auto* definition = match.weapons_[source.unit_index].definitions[index];
        if (!definition)
            return false;
        const auto& def = match_unit_def(match, source.record);
        const auto& record = match.state().game.weapon_defs[definition->registry_index];
        return sim::ballistics::fire_can_reach(
            {definition->flags,
             definition->range_world_units,
             sim::weapon_execution::ballistic_parameters(
                 definition->projectile_velocity,
                 definition->minimum_barrel_angle_radians,
                 match.state().game.gravity,
                 rules.rules()
             ),
             sim::weapon_execution::reach_keys(rules.weapon(record.weapon_id))},
            {from.fixed, high_word(static_cast<uint32_t>(def.model_height)), 0, 0},
            target.point.fixed,
            match.simulation_.sea_level
        );
    }

    bool fire_projectile(uint8_t index, const sim::weapon_execution::Target& target) override {
        const auto* definition = match.weapons_[source.unit_index].definitions[index];
        if (!definition)
            return false;
        auto* instance = match.instance(source.unit_index);
        // The muzzle is the QueryWeapon piece in world space.
        std::array<uint32_t, 3> muzzle = source.unit->position;
        uint16_t query_piece = 0;
        if (instance && instance->script()) {
            const auto piece = instance->query_weapon_piece(index);
            query_piece = static_cast<uint16_t>(piece);
            muzzle = instance->piece_world(piece);
        }
        const auto goal = as_position(target.point);
        // The muzzle offset still lives in side state; mirror it into the
        // canonical weapon slot the constructors read.
        auto& slot = source.record.weapons[index];
        sim::weapon_execution::set_weapon_muzzle_offset(
            slot, match.weapons_[source.unit_index].slots[index].muzzle_offset
        );
        const oa::FixedVec3 muzzle_point{
            signed_word(muzzle[0]), signed_word(muzzle[1]), signed_word(muzzle[2])
        };
        const oa::FixedVec3 target_point{
            signed_word(goal[0]), signed_word(goal[1]), signed_word(goal[2])
        };
        const auto* ground = match.ground_runtime(source.unit_index);
        const auto random = [](void* context, uint32_t limit) {
            return static_cast<Match*>(context)->random_bounded(limit);
        };
        auto& world = match.state();
        const auto plan = sim::weapon_execution::plan_weapon_shot(
            world.game.weapon_defs[definition->registry_index],
            source.record,
            *oa::world_unit_def_of(&world, &source.record),
            index,
            {muzzle_point.x, muzzle_point.y, muzzle_point.z},
            {target_point.x, target_point.y, target_point.z},
            ground ? ground->movement.speed : 0,
            world,
            random,
            &match,
            rules
        );
        slot.aim_heading = plan.slot_aim.heading;
        slot.aim_pitch = plan.slot_aim.pitch;
        if (!plan.fired)
            return false;
        auto* shot = sim::weapon_execution::allocate_projectile(world);
        if (shot == nullptr)
            return false;
        const auto target_unit = static_cast<uint16_t>(target.unit_identity);
        match.place_shot(
            *shot,
            source,
            index,
            plan.launch,
            muzzle_point,
            &target_point,
            target_unit,
            plan.intercept_target,
            query_piece
        );
        if (plan.spends_aim)
            slot.aim_ready = 0;
        if (plan.fire_script)
            match.run_fire_scripts(source, index, plan.slot_aim.heading, muzzle_point);
        // The shot is then shared, a fixed line weapon's with the unit's
        // position as the start.
        ShotEvent shared{};
        shared.start = sim::weapon_execution::select_fire_mode(
                           definition->flags, rules.rules().weapons.vlaunch_before_turret.enabled
                       ) == sim::weapon_execution::FireMode::line
                           ? source.record.position
                           : muzzle_point;
        shared.target = target_point;
        const auto& weapon = world.game.weapon_defs[definition->registry_index];
        shared.weapon_id = weapon.weapon_id;
        shared.interceptor = (weapon.flags & OA_WEAPON_FLAG_INTERCEPTOR) != 0;
        shared.aim_heading = slot.aim_heading;
        shared.aim_pitch = slot.aim_pitch;
        shared.target_unit = target_unit;
        shared.source_unit = source.unit_index;
        shared.slot = index;
        match.share_shot(shared);
        return true;
    }

    void stockpile_consumed(uint8_t) override {}

    oa::Player& owner() {
        return match_player(match, source.unit->owner ? source.unit->owner->record.index : 0);
    }

    bool can_pay_shot_cost(float energy, float metal) override {
        const auto& player = owner();
        return energy <= player.energy && metal <= player.metal;
    }

    void pay_shot_cost(float energy, float metal) override {
        // The weapon tick pays through the unit's economy block
        // (Unit.economy) and ignores the result.
        auto& economy = source.record.economy;
        (void)sim::unit_spawn::player_pay_resources(
            owner(), economy.energy.requested, economy.metal.requested, energy, metal
        );
    }
};

void Match::place_shot(
    oa::Projectile& shot,
    sim::unit_spawn::Slot& source,
    uint8_t slot,
    const sim::weapon_execution::ProjectileLaunch& launch,
    const FixedVec3& start,
    const FixedVec3* target,
    uint16_t target_unit,
    oa_ref32 intercept_target,
    uint16_t query_piece
) {
    const auto& definition = *weapons_[source.unit_index].definitions[slot];
    auto& world = state();
    sim::weapon_execution::init_projectile_record(
        world,
        shot,
        oa::oa_ref_from_index(definition.registry_index),
        start,
        target,
        world.game.tick,
        &source.record,
        query_piece
    );
    shot.heading = launch.heading;
    shot.pitch = launch.pitch;
    shot.speed = launch.speed;
    shot.distance = launch.distance;
    shot.velocity = {launch.velocity[0], launch.velocity[1], launch.velocity[2]};
    shot.lifetime_tick = launch.lifetime_tick;
    shot.burst_remaining = launch.burst_remaining;
    shot.target_unit = oa::oa_unit_ref_from_slot(target_unit);
    shot.intercept_target = intercept_target;
    // A shot apply_shot places reports the aim and target unit it was fired
    // with, which the record may not keep. The pending shot is cleared
    // whether or not anyone listens.
    const auto* applied = std::exchange(applied_shot_, nullptr);
    if (event_wanted<&EventHooks::shot_placed>()) {
        const auto* aim = applied != nullptr ? &applied->target : target;
        const auto aimed_unit =
            applied != nullptr
                ? (applied->target_unit < slots_.size() ? applied->target_unit : uint16_t{})
                : target_unit;
        emit_event<&EventHooks::shot_placed>(world, shot, ShotSource::weapon, aim, aimed_unit);
    }
    play_sound_at(definition.soundstart.c_str(), start);
}

void Match::run_fire_scripts(
    sim::unit_spawn::Slot& source, uint8_t slot, int16_t heading, const FixedVec3& start
) {
    auto* unit = instance(source.unit_index);
    if (auto* cob = unit != nullptr ? unit->script() : nullptr) {
        cob->call_no_arguments(fire_script_names[slot], false);
        const auto rock = sim::weapon_execution::rock_unit_arguments(
            source.record, std::bit_cast<uint16_t>(heading)
        );
        cob->call("RockUnit", rock, false);
    }
    const auto* definition = weapons_[source.unit_index].definitions[slot];
    if (definition != nullptr &&
        (definition->flags & sim::combat_state::weapon_start_smoke_flag) != 0)
        sim::effect_particles::spawn_start_smoke(
            effects(), state().game, effect_host(), start, sim::effect_particles::layer_smoke
        );
}

void Match::share_shot(const ShotEvent& shot) {
    if (multiplayer.shot_fired != nullptr)
        multiplayer.shot_fired(multiplayer.context, shot);
}

void Match::apply_shot(const ShotEvent& shot) {
    auto& world = state();
    const auto weapon_ref = oa::oa_ref_from_index(shot.weapon_id);
    const auto* weapon = oa::world_weapon_def(&world, weapon_ref);
    if (weapon == nullptr)
        return;
    if ((weapon->flags & OA_WEAPON_FLAG_METEOR) != 0) {
        (void)launch_meteor(weapon_ref, shot.start, shot.target, false);
        return;
    }
    // The event has room for a fourth slot the unit does not have.
    if (shot.source_unit == 0 || shot.source_unit >= slots_.size() ||
        shot.slot >= OA_UNIT_WEAPON_COUNT)
        return;
    auto& source = slots_[shot.source_unit];
    if (source.unit == nullptr || (source.record.flags & OA_UNIT_FLAG_LIVE) == 0)
        return;
    auto& aimed = source.record.weapons[shot.slot];
    aimed.aim_pitch = shot.aim_pitch;
    aimed.aim_heading = shot.aim_heading;
    const auto* definition = weapons_[shot.source_unit].definitions[shot.slot];
    if (definition == nullptr)
        return;
    // The constructors launch the slot's weapon; the carried one only picks them.
    const auto& slot_weapon = world.game.weapon_defs[definition->registry_index];
    const uint16_t target_unit = shot.target_unit < slots_.size() ? shot.target_unit : 0;
    const sim::weapon_execution::FixedVector start{shot.start.x, shot.start.y, shot.start.z};
    const sim::weapon_execution::FixedVector target{shot.target.x, shot.target.y, shot.target.z};
    const auto tick = world.game.tick;
    sim::weapon_execution::ProjectileLaunch launch{};
    const FixedVec3* aim_point = &shot.target;
    uint16_t linked_unit = target_unit;
    oa_ref32 intercept = 0;
    bool fire_scripts = true;
    if ((weapon->flags & OA_WEAPON_FLAG_BALLISTIC) != 0) {
        sim::weapon_execution::set_weapon_muzzle_offset(
            aimed, weapons_[shot.source_unit].slots[shot.slot].muzzle_offset
        );
        launch = sim::weapon_execution::launch_ballistic_projectile(
            slot_weapon, aimed, world.game.gravity, start, target, tick
        );
        aim_point = nullptr;
    } else if ((weapon->flags & OA_WEAPON_FLAG_VLAUNCH) != 0) {
        launch = sim::weapon_execution::launch_vertical_projectile(slot_weapon, tick);
        if (shot.interceptor)
            intercept = sim::weapon_execution::find_intercept_target_fired_by(
                world, shot.target, shot.target_unit
            );
    } else if ((weapon->flags & (OA_WEAPON_FLAG_LINE_OF_SIGHT | OA_WEAPON_FLAG_SELF_PROP)) != 0) {
        launch = sim::weapon_execution::launch_line_projectile(slot_weapon, start, target, tick);
    } else if ((weapon->flags & OA_WEAPON_FLAG_DROPPED) != 0) {
        const auto* ground = ground_runtime(shot.source_unit);
        launch = sim::weapon_execution::launch_dropped_projectile(
            source.record, ground ? ground->movement.speed : 0
        );
        aim_point = nullptr;
        linked_unit = 0;
        fire_scripts = false;
    } else {
        return;
    }
    auto* projectile = sim::weapon_execution::allocate_projectile(world);
    if (projectile == nullptr)
        return;
    // The shot's query piece is that of the first slot carrying the weapon.
    uint8_t query_slot = 0;
    while (query_slot < OA_UNIT_WEAPON_COUNT &&
           weapons_[shot.source_unit].definitions[query_slot] != definition)
        ++query_slot;
    uint16_t query_piece = 0;
    auto* unit = instance(shot.source_unit);
    if (unit != nullptr && unit->script() != nullptr && query_slot < OA_UNIT_WEAPON_COUNT)
        query_piece = static_cast<uint16_t>(unit->query_weapon_piece(query_slot));
    applied_shot_ = &shot;
    place_shot(
        *projectile,
        source,
        shot.slot,
        launch,
        shot.start,
        aim_point,
        linked_unit,
        intercept,
        query_piece
    );
    if (fire_scripts)
        run_fire_scripts(source, shot.slot, aimed.aim_heading, shot.start);
}

void TickHost::tick_weapon_aim(oa::Unit& record) {
    auto& u = unit_view(record);
    auto& s = slot(u);
    auto& weapons = match.weapons_[s.unit_index];
    auto& slots = s.record.weapons;
    std::array<sim::weapon_execution::WeaponDefinition, 3> definitions{};
    sim::weapon_execution::UnitState state;
    state.position = as_point(u.position);
    state.veteran_level = s.record.veteran_level;
    state.type_index = s.record.type_index;
    state.health = u.health;
    state.maximum_health = u.type->maximum_health ? u.type->maximum_health : 1;
    state.shot_event_bits = u.events;
    for (std::size_t i = 0; i < 3; ++i) {
        auto& weapon = weapons.slots[i];
        weapon.flags = slots[i].flags;
        const auto* source = weapon.definition ? weapon.definition : weapons.definitions[i];
        if (source) {
            definitions[i].projectile_constructor_present = source->registry_index != 0;
            definitions[i].flags = source->flags;
            definitions[i].base_reload_ticks = source->reload_time_ticks;
            definitions[i].range_fixed = source->range_world_units;
            definitions[i].energy_per_shot = source->energy_per_shot;
            definitions[i].metal_per_shot = source->metal_per_shot;
        }
        state.slots[i] = {
            source ? &definitions[i] : nullptr, &slots[i], slots[i].stockpile, weapon.flags
        };
    }
    WeaponTickHost host(match, s);
    (void)sim::weapon_execution::tick_weapons(state, host);
    for (std::size_t i = 0; i < 3; ++i) {
        auto& weapon = weapons.slots[i];
        s.record.weapons[i].stockpile = state.slots[i].stockpile_count;
        weapon.flags = state.slots[i].flags;
        slots[i].flags = state.slots[i].flags;
    }
    u.events = state.shot_event_bits;
}

void TickHost::wake_weapon(sim::simulation_state::Unit& u, uint32_t index) {
    before_callback();
    match.stop_weapon(slot(u), index);
    after_callback();
}

} // namespace oa::sim::match_runtime
