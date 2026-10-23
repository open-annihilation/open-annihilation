// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "tick_internal.hpp"
#include "oa/sim/weapon_execution/interceptor.hpp"
#include "oa/sim/weapon_execution/projectile_contact.hpp"
#include "oa/sim/weapon_execution/projectile_pool.hpp"
#include "oa/sim/weapon_execution/weapon_keys.hpp"
#include "oa/sim/unit_movement/terrain.hpp"

#include <cstring>

namespace oa::sim::match_runtime {

namespace {

/// Tells whether a burst spawner's target field holds the point it was aimed
/// at. A mirrored player's ballistic or dropped shot, which apply_shot
/// launches without its aim (the constructors picked in apply_shot's order),
/// keeps an earlier shot's point there.
///
/// @param world the match's world
/// @param shot the spawner
/// @param flags its weapon's flags
/// @return false for such a shot
bool spawner_keeps_aim(const oa::World& world, const oa::Projectile& shot, uint32_t flags) {
    const auto* owner = oa::world_player(&world, shot.owner_index);
    if (owner == nullptr || owner->in_use == 0 || owner->status != OA_PLAYER_STATUS_MIRRORED)
        return true;
    if ((flags & OA_WEAPON_FLAG_BALLISTIC) != 0)
        return false;
    if ((flags &
         (OA_WEAPON_FLAG_VLAUNCH | OA_WEAPON_FLAG_LINE_OF_SIGHT | OA_WEAPON_FLAG_SELF_PROP)) != 0)
        return true;
    return (flags & OA_WEAPON_FLAG_DROPPED) == 0;
}

/// Finds the height of the feature the shot contact test finds under a plot:
/// a direct feature word only within the FeatureDef table, a continuation
/// through its footprint origin's word without that check.
///
/// The height is FeatureDef.height; a match given no table reads the height
/// the map loader resolved into the origin plot.
///
/// @param world World whose FeatureDef table names the features.
/// @param plots The match's plots.
/// @param index Plot to test.
/// @param width Map width in plots, for the continuation's walk back.
/// @param[out] height The feature's height, set only on success.
/// @return True when the plot holds a feature with a height.
bool plot_feature_height(
    const oa::World& world,
    const std::vector<sim::spatial_state::Plot>& plots,
    std::size_t index,
    uint32_t width,
    uint8_t& height
) {
    const bool table = world.feature_defs != nullptr;
    auto origin = index;
    auto word = plots[index].feature_word;
    if (word < sim::spatial_state::first_reserved_feature) {
        if (table && static_cast<int32_t>(word) >= world.game.feature_def_count)
            return false;
    } else {
        if (word != sim::spatial_state::feature_continuation)
            return false;
        const auto back = static_cast<std::size_t>(plots[index].feature_back_z) * width +
                          plots[index].feature_back_x;
        if (back > index)
            return false;
        origin = index - back;
        word = plots[origin].feature_word;
        if (word >= sim::spatial_state::first_reserved_feature)
            return false;
    }
    if (!table) {
        height = plots[origin].feature_height;
        return true;
    }
    // A continuation's word outside the table names no feature.
    if (word >= world.feature_def_count)
        return false;
    height = static_cast<uint8_t>(world.feature_defs[word].height);
    return true;
}

/// Tells whether a ballistic shell explodes when its weapontimer runs out.
///
/// 3.1c explodes it only when the weapon has burnblow; under
/// weapons.timed-shell-detonation's not-noautorange rule it explodes unless the
/// weapon has noautorange. Otherwise it goes out in a light puff.
///
/// @param flags WeaponDef.flags
/// @param timed the hack's rules
/// @return true when the shell explodes
bool timed_shell_explodes(
    uint32_t flags, const data::match_rules::WeaponsTimedShellDetonation& timed
) noexcept {
    if (timed.enabled &&
        timed.rule == data::match_rules::WeaponsTimedShellDetonationRule::not_noautorange)
        return (flags & OA_WEAPON_FLAG_NO_AUTO_RANGE) == 0;
    return (flags & OA_WEAPON_FLAG_BURN_BLOW) != 0;
}

} // namespace

class ProjectileDamageHost final : public sim::unit_health::DamageHost {
    Match& match;

    sim::unit_spawn::Slot& slot(const sim::unit_health::Unit& u) {
        return match.spawn_slot(u.identity);
    }

  public:

    explicit ProjectileDamageHost(Match& world) : match(world) { rules = world.rules_view(); }

    bool target_is_live(const sim::unit_health::Unit& u) override {
        return unit_is_live_target(slot(u).unit->flags);
    }

    void apply_health_event(
        sim::unit_health::Unit& target,
        const sim::unit_health::Unit* source,
        const sim::unit_health::HealthEvent& event
    ) override {
        match.apply_damage_event(
            slot(target),
            source ? &slot(*source) : nullptr,
            event.amount,
            event.kind,
            event.direction
        );
    }

    bool target_owner_present(const sim::unit_health::Unit& u) override {
        return slot(u).unit->owner && slot(u).unit->owner->present;
    }

    uint8_t target_owner_status(const sim::unit_health::Unit& u) override {
        return slot(u).unit->owner ? slot(u).unit->owner->status : 0;
    }

    sim::unit_health::RouteIdentity source_owner_route(const sim::unit_health::Unit& u) override {
        const auto& multiplayer = match.multiplayer;
        if (multiplayer.health_route)
            return multiplayer.health_route(multiplayer.context, u.identity);
        match.note_fault("health event route without a multiplayer handler");
        return {};
    }

    sim::unit_health::RouteIdentity fallback_route() override {
        const auto& multiplayer = match.multiplayer;
        if (multiplayer.health_route)
            return multiplayer.health_route(multiplayer.context, 0);
        match.note_fault("health event fallback route without a multiplayer handler");
        return {};
    }

    void share_health_event(
        sim::unit_health::RouteIdentity route, const sim::unit_health::HealthEvent& event
    ) override {
        const auto& multiplayer = match.multiplayer;
        if (multiplayer.health_shared) {
            multiplayer.health_shared(multiplayer.context, route, event);
            return;
        }
        match.note_fault("health event shared without a multiplayer handler");
    }
};

void Match::explode_unit(sim::unit_spawn::Slot& slot, bool self_destruct) {
    const auto& definition = *fields(slot).definition;
    const auto* weapon =
        input_.weapons.find(self_destruct ? definition.self_destruct_as : definition.explode_as);
    if (!weapon)
        return;
    // The stand-in fills only the weapon, both positions and the owner.
    oa::Projectile blast{};
    blast.def = oa::oa_ref_from_index(weapon->registry_index);
    blast.position = slot.record.position;
    blast.origin = slot.record.position;
    blast.owner_index = slot.record.owner_index;
    detonate(blast, nullptr);
}

int32_t
Match::apply_projectile_damage(oa::Projectile& shot, sim::unit_spawn::Slot& target, float scale) {
    const auto* definition = projectile_weapon(shot);
    if (!definition) {
        fault_.note("projectile has no weapon definition");
        return 0;
    }
    // The DAMAGE entry for the target's UNITNAME, else the default.
    const auto* target_definition = fields(target).definition;
    const auto base =
        target_definition
            ? sim::combat_state::damage_against(*definition, target_definition->unit_name)
            : static_cast<int32_t>(definition->default_damage);
    auto* source = projectile_source(shot);
    const auto amount = sim::weapon_execution::projectile_damage(
        base, scale, source ? &source->record : nullptr, state().game, rules_view()
    );
    const auto direction = sim::weapon_execution::impact_direction(
        {shot.position.x, 0, shot.position.z}, target.record
    );
    const auto kind = 1u + ((definition->flags & sim::combat_state::weapon_paralyzer_flag) != 0);
    sim::unit_health::UnitType type{
        fields(target).definition->damage_modifier_fixed,
        static_cast<float>(fields(target).definition->build_cost_energy),
        fields(target).definition->build_time,
        target.unit->type->maximum_health
    };
    sim::unit_health::Unit projected{
        target.unit_index,
        target.record.state_flags,
        target.record.veteran_level,
        target.unit->health,
        &type
    };
    projected.type_index = target.record.type_index;
    sim::unit_health::Unit source_projected{};
    const sim::unit_health::Unit* source_ptr = nullptr;
    if (source) {
        source_projected = {
            source->unit_index,
            source->record.state_flags,
            source->record.veteran_level,
            source->unit->health,
            &type
        };
        source_ptr = &source_projected;
    }
    ProjectileDamageHost damage(*this);
    sim::unit_health::submit_damage(source_ptr, projected, amount, kind, damage, direction);
    return amount;
}

int32_t Match::strike_unit(oa::Projectile& shot, sim::unit_spawn::Slot& target) {
    const auto amount = apply_projectile_damage(shot, target);
    if (auto* source = projectile_source(shot)) {
        if (shot.owner_index != target.record.owner_index)
            record_shot_reaction(*source, amount, 0);
        else
            record_shot_reaction(*source, 0, amount);
    }
    return amount;
}

void Match::record_shot_reaction(sim::unit_spawn::Slot& source, int32_t enemy, int32_t friendly) {
    // The reaction is the high byte of Unit.events.
    auto reaction = static_cast<uint8_t>(source.record.events >> 8);
    sim::unit_health::record_hit_reaction(reaction, enemy, friendly);
    source.record.events = static_cast<uint16_t>(
        (source.record.events & 0x00ffu) | (static_cast<uint16_t>(reaction) << 8)
    );
}

sim::unit_spawn::Slot* Match::projectile_source(const oa::Projectile& shot) {
    const auto index = oa::oa_unit_slot_from_ref(shot.source);
    return index != 0 && index < slots_.size() ? &slots_[index] : nullptr;
}

void Match::detonate(oa::Projectile& shot, sim::unit_spawn::Slot* direct) {
    auto& world = state();
    if (event_wanted<&EventHooks::shot_detonated>()) {
        const auto struck = direct != nullptr ? direct->unit_index : uint16_t{};
        emit_event<&EventHooks::shot_detonated>(world, shot, struck);
    }
    const auto* definition = projectile_weapon(shot);
    const auto* weapon = oa::world_weapon_def(&world, shot.def);
    if (!definition || !weapon) {
        sim::weapon_execution::retire_projectile(world, shot);
        return;
    }
    // A noexplode shot stays live through its blasts; the retired bit also
    // keeps a chained blast from detonating this shot again.
    if ((weapon->flags & OA_WEAPON_FLAG_NO_EXPLODE) == 0)
        sim::weapon_execution::retire_projectile(world, shot);
    // A shot a nosealeveltrigger sea swallows is retired even when noexplode,
    // and neither sounds nor damages.
    if (!spawn_weapon_explosion(*weapon, fixed_words(shot.position), direct != nullptr)) {
        sim::weapon_execution::retire_projectile(world, shot);
        return;
    }
    // A mirrored player's shot damages nothing here; its own machine applies
    // the damage and shares it.
    if (const auto* owner = oa::world_player(&world, shot.owner_index);
        owner != nullptr && owner->in_use != 0 && owner->status == OA_PLAYER_STATUS_MIRRORED)
        return;
    if (direct != nullptr && definition->areaofeffect < 0x11)
        strike_unit(shot, *direct);
    else if (!sim::weapon_execution::silent_ownerless_shot(
                 shot, *weapon, rules_view().weapon(weapon->weapon_id)
             ))
        detonate_area(shot, *definition, *weapon);
}

void Match::detonate_area(
    oa::Projectile& shot,
    const sim::combat_state::WeaponDefinition& definition,
    const oa::WeaponDef& weapon
) {
    auto& world = state();
    auto* source = projectile_source(shot);
    const auto radius = static_cast<int32_t>(definition.areaofeffect >> 1);
    const auto pad = radius / 16 + 1;
    const auto center_x = static_cast<int32_t>(static_cast<int16_t>(shot.position.x >> 16)) / 16;
    const auto center_z = static_cast<int32_t>(static_cast<int16_t>(shot.position.z >> 16)) / 16;
    const auto width = static_cast<int32_t>(spatial_.terrain_width);
    const auto height = static_cast<int32_t>(spatial_.terrain_height);
    auto x0 = center_x - pad;
    auto x1 = center_x + pad;
    auto z0 = center_z - pad;
    auto z1 = center_z + pad;
    if (x0 < 0)
        x0 = 0;
    if (z0 < 0)
        z0 = 0;
    if (x1 > width)
        x1 = width;
    if (z1 > height)
        z1 = height;
    sim::weapon_execution::AreaBlastVisits visits{};
    auto enemy = 0;
    auto friendly = 0;
    for (auto z = z0; z < z1; ++z) {
        for (auto x = x0; x < x1; ++x) {
            const auto index = static_cast<std::size_t>(z) * static_cast<std::size_t>(width) +
                               static_cast<std::size_t>(x);
            if (index >= spatial_.plots.size())
                continue;
            const auto& plot = spatial_.plots[index];
            const uint16_t occupants[2] = {plot.ground, plot.air};
            for (const auto id : occupants) {
                if (id == 0 || id >= slots_.size() || slots_[id].unit == nullptr)
                    continue;
                auto& victim = slots_[id];
                if (&victim == source ||
                    !sim::weapon_execution::note_blast_unit(visits, victim.record))
                    continue;
                const auto* bounds = bounds_for(*victim.unit);
                const auto& at = victim.record.position;
                const auto reach = bounds ? sim::weapon_execution::blast_reach_to_box(
                                                {shot.position.x, shot.position.y, shot.position.z},
                                                {wrapping_add(at.x, bounds->bounds_min_x),
                                                 wrapping_add(at.y, bounds->bounds_min_y),
                                                 wrapping_add(at.z, bounds->bounds_min_z)},
                                                {wrapping_add(at.x, bounds->bounds_max_x),
                                                 wrapping_add(at.y, bounds->model_height),
                                                 wrapping_add(at.z, bounds->bounds_max_z)}
                                            )
                                          : base::game_math::truncated_length(
                                                wrapping_sub(shot.position.x, at.x),
                                                wrapping_sub(shot.position.y, at.y),
                                                wrapping_sub(shot.position.z, at.z)
                                            ) >> 16;
                if (reach >= radius)
                    continue;
                // Edge effectiveness falls off with the reach.
                const auto amount = apply_projectile_damage(
                    shot, victim, sim::weapon_execution::area_damage_scale(weapon, reach, radius)
                );
                if (victim.record.owner_index == shot.owner_index)
                    friendly += amount;
                else
                    enemy += amount;
            }
            // weapon_ground_skip_flag skips features. A continuation walks back
            // to the origin. Only the origin cell of a record-bearing feature
            // measures from the record's position (PlacedFeature.model.position,
            // whatever bytes a sprite record keeps there); every other cell
            // measures from the footprint centre feature placement uses at
            // that cell. The blast records at most 0x40 origins; a full set
            // still damages.
            if ((definition.flags & sim::combat_state::weapon_ground_skip_flag) != 0)
                continue;
            const auto& canonical = world.plots[index];
            auto origin = index;
            auto origin_x = x;
            auto origin_z = z;
            if (canonical.feature == sim::feature_runtime::feature_continuation) {
                origin_x = x - static_cast<int32_t>(canonical.feature_record >> 8);
                origin_z = z - static_cast<int32_t>(canonical.feature_record & 0xffu);
                const auto* at = oa::world_plot(&world, origin_x, origin_z);
                if (at == nullptr)
                    continue;
                origin = static_cast<std::size_t>(at - world.plots);
            }
            const auto word = world.plots[origin].feature;
            if (word >= OA_PLOT_FEATURE_RESERVED || word >= world.feature_def_count)
                continue;
            FixedVec3 point{};
            if ((canonical.flags & OA_PLOT_FLAG_ANIMATING_FEATURE) != 0) {
                const auto* record =
                    sim::feature_runtime::feature_record(world, canonical.feature_record);
                if (record == nullptr)
                    continue;
                std::memcpy(&point, &record->model.position, sizeof point);
            } else {
                point = sim::feature_runtime::feature_center(
                    world,
                    static_cast<int16_t>(x),
                    static_cast<int16_t>(z),
                    world.feature_defs[word]
                );
            }
            const auto reach = static_cast<int32_t>(static_cast<int16_t>(
                base::game_math::truncated_length(
                    wrapping_sub(shot.position.x, point.x),
                    wrapping_sub(shot.position.y, point.y),
                    wrapping_sub(shot.position.z, point.z)
                ) >>
                16
            ));
            if (reach >= radius ||
                !sim::weapon_execution::note_blast_feature(visits, static_cast<uint32_t>(origin)))
                continue;
            sim::feature_runtime::damage_feature(
                world, feature_host(), origin, origin_x, origin_z, weapon
            );
        }
    }
    // An interceptor's blast detonates every other live shot inside it, and
    // shares each one it set off once it has gone off here.
    if ((definition.flags & OA_WEAPON_FLAG_INTERCEPTOR) != 0) {
        const auto reach = static_cast<uint32_t>(definition.areaofeffect) * definition.areaofeffect;
        for (int32_t index = 0;
             index < world.game.projectile_count && index < OA_PROJECTILE_CAPACITY;
             ++index) {
            auto& other = world.projectiles[index];
            if (&other == &shot || (other.flags & OA_PROJECTILE_FLAG_RETIRED) != 0)
                continue;
            const auto dx = wrapping_sub(shot.position.x, other.position.x);
            const auto dy = wrapping_sub(shot.position.y, other.position.y);
            const auto dz = wrapping_sub(shot.position.z, other.position.z);
            if (base::game_math::squared_magnitude_high(dx, dy, dz) >= static_cast<int32_t>(reach))
                continue;
            detonate(other, nullptr);
            if (multiplayer.shot_intercepted != nullptr)
                multiplayer.shot_intercepted(multiplayer.context, other, shot);
        }
    }
    if (source)
        record_shot_reaction(*source, enemy, friendly);
}

const sim::spatial_state::Plot* Match::plot_at(int32_t x, int32_t z) const {
    const auto width = static_cast<int32_t>(spatial_.terrain_width);
    int32_t cell_x = 0, cell_z = 0;
    if (!sim::spatial_state::position_to_cell(
            x, z, width, static_cast<int32_t>(spatial_.terrain_height), cell_x, cell_z
        ))
        return nullptr;
    const auto index = static_cast<std::size_t>(cell_z) * static_cast<std::size_t>(width) +
                       static_cast<std::size_t>(cell_x);
    return index < spatial_.plots.size() ? &spatial_.plots[index] : nullptr;
}

void Match::resolve_projectile_contact(oa::Projectile& shot) {
    auto& world = state();
    sim::weapon_execution::ContactPlot cell;
    const sim::weapon_execution::ContactPlot* under = nullptr;
    if (const auto* plot = plot_at(shot.position.x, shot.position.z)) {
        cell.ground_unit = plot->ground;
        cell.air_unit = plot->air;
        cell.high_height = plot->high_height;
        cell.low_height = plot->low_height;
        cell.has_feature = plot_feature_height(
            world,
            spatial_.plots,
            static_cast<std::size_t>(plot - spatial_.plots.data()),
            spatial_.terrain_width,
            cell.feature_height
        );
        under = &cell;
    }
    const auto contact = sim::weapon_execution::projectile_plot_contact(world, shot, under, false);
    if (contact.intercepted)
        detonate(shot, nullptr);
    switch (contact.kind) {
    case sim::weapon_execution::ContactKind::none:
    case sim::weapon_execution::ContactKind::bounce:
        return;
    case sim::weapon_execution::ContactKind::off_map:
        sim::weapon_execution::retire_projectile(world, shot);
        return;
    case sim::weapon_execution::ContactKind::unit: {
        const auto slot = oa::oa_unit_slot_from_ref(contact.unit);
        detonate(shot, slot < slots_.size() ? &slots_[slot] : nullptr);
        return;
    }
    case sim::weapon_execution::ContactKind::feature:
    case sim::weapon_execution::ContactKind::ground:
    case sim::weapon_execution::ContactKind::water:
        detonate(shot, nullptr);
        return;
    }
}

void Match::update_projectiles() {
    auto& world = state();
    const auto tick = world.game.tick;
    const auto gravity = world.game.gravity;
    const auto sea = static_cast<int16_t>(world.game.sea_level);
    const auto surface = [](void* context, int32_t x, int32_t z) {
        return static_cast<int32_t>(
            sim::unit_movement::surface_height(static_cast<Match*>(context)->terrain_, x, z)
        );
    };
    // The pass walks the count it started with by position. Shots retired
    // earlier in the pass still fly and collide, and a compaction inside the
    // pass shifts later records under the running index.
    auto remaining = world.game.projectile_count;
    for (int32_t index = 0; remaining > 0 && index < OA_PROJECTILE_CAPACITY; ++index, --remaining) {
        auto& shot = world.projectiles[index];
        const auto* weapon = oa::world_weapon_def(&world, shot.def);
        if (weapon == nullptr) {
            sim::weapon_execution::retire_projectile(world, shot);
            continue;
        }
        const auto flags = weapon->flags;
        // A record with burst shots left is a spawner. It does not
        // fly; every burst_rate ticks it copies itself as one flying child.
        if (shot.burst_remaining != 0) {
            const auto rate = static_cast<uint32_t>(static_cast<uint16_t>(weapon->burst_rate));
            if (rate + shot.burst_tick > tick)
                continue;
            auto* source = projectile_source(shot);
            if ((rate > 4 || (shot.burst_remaining & 1) != 0) && source != nullptr)
                if (auto* launcher = instance(source->unit_index)) {
                    const auto muzzle = launcher->piece_world(shot.query_piece);
                    shot.position = {
                        signed_word(muzzle[0]), signed_word(muzzle[1]), signed_word(muzzle[2])
                    };
                }
            --shot.burst_remaining;
            shot.burst_tick += rate;
            if (auto* child = sim::weapon_execution::allocate_projectile(world)) {
                *child = shot;
                child->burst_tick = tick;
                const auto* definition = projectile_weapon(shot);
                if ((flags & OA_WEAPON_FLAG_SOUND_TRIGGER) != 0 && definition != nullptr)
                    play_sound_at(definition->soundstart.c_str(), shot.position);
                const auto random = [](void* context, uint32_t limit) {
                    return static_cast<Match*>(context)->random_bounded(limit);
                };
                const auto burst = sim::weapon_execution::burst_child(
                    *weapon,
                    shot.distance,
                    shot.speed,
                    shot.heading,
                    shot.pitch,
                    {shot.velocity.x, shot.velocity.y, shot.velocity.z},
                    tick,
                    random,
                    this
                );
                child->lifetime_tick = burst.lifetime_tick;
                child->burst_remaining = 0;
                shot.velocity = {
                    burst.parent_velocity[0], burst.parent_velocity[1], burst.parent_velocity[2]
                };
                if (event_wanted<&EventHooks::shot_placed>()) {
                    const auto aimed_unit = oa::oa_unit_slot_from_ref(child->target_unit);
                    emit_event<&EventHooks::shot_placed>(
                        world,
                        *child,
                        ShotSource::burst,
                        spawner_keeps_aim(world, *child, flags) ? &child->target : nullptr,
                        aimed_unit < slots_.size() ? static_cast<uint16_t>(aimed_unit) : uint16_t{}
                    );
                }
            }
            if (shot.burst_remaining == 0)
                sim::weapon_execution::retire_projectile(world, shot);
            continue;
        }
        const auto previous_height =
            static_cast<int16_t>(static_cast<uint32_t>(shot.position.y) >> 16);
        const auto move = [&shot]() {
            shot.position.x = wrapping_add(shot.position.x, shot.velocity.x);
            shot.position.y = wrapping_add(shot.position.y, shot.velocity.y);
            shot.position.z = wrapping_add(shot.position.z, shot.velocity.z);
        };
        const auto fall_with_wind = [&]() {
            move();
            shot.position.x = wrapping_add(shot.position.x, environment_wind_.vector_x);
            shot.position.z = wrapping_add(shot.position.z, environment_wind_.vector_z);
            shot.velocity.y = wrapping_sub(shot.velocity.y, gravity);
        };
        auto collides = true;
        switch (sim::weapon_execution::flight_mode(*weapon)) {
        case sim::weapon_execution::FlightMode::inert:
            collides = false;
            break;
        case sim::weapon_execution::FlightMode::line:
            if (shot.lifetime_tick <= tick) {
                sim::weapon_execution::retire_projectile(world, shot);
                collides = false;
                break;
            }
            move();
            if ((flags & OA_WEAPON_FLAG_BEAM_WEAPON) != 0) {
                // The beam's tail starts following its head after `duration`.
                if ((shot.flags & OA_PROJECTILE_FLAG_BEAM_TAIL) == 0) {
                    if (static_cast<uint32_t>(static_cast<uint16_t>(weapon->duration)) +
                            shot.burst_tick <
                        tick)
                        shot.flags =
                            static_cast<uint16_t>(shot.flags | OA_PROJECTILE_FLAG_BEAM_TAIL);
                } else {
                    shot.origin.x = wrapping_add(shot.origin.x, shot.velocity.x);
                    shot.origin.y = wrapping_add(shot.origin.y, shot.velocity.y);
                    shot.origin.z = wrapping_add(shot.origin.z, shot.velocity.z);
                }
            }
            break;
        case sim::weapon_execution::FlightMode::ballistic:
            if (weapon->weapon_timer == 0 || tick < shot.lifetime_tick) {
                fall_with_wind();
                break;
            }
            if (timed_shell_explodes(flags, rules().weapons.timed_shell_detonation)) {
                detonate(shot, nullptr);
            } else {
                // A timed shell that does not explode goes out in a light puff.
                spawn_light_puff(shot.position);
                sim::weapon_execution::retire_projectile(world, shot);
            }
            collides = false;
            break;
        case sim::weapon_execution::FlightMode::dropped:
            fall_with_wind();
            break;
        case sim::weapon_execution::FlightMode::meteor:
            move();
            break;
        case sim::weapon_execution::FlightMode::self_propelled:
            if (tick < shot.lifetime_tick) {
                // A water weapon's shot above sea level falls, unless it
                // has the surface-fire key.
                if ((flags & OA_WEAPON_FLAG_WATER_WEAPON) == 0 || previous_height < sea ||
                    rules_view().weapon(weapon->weapon_id).surface_fire) {
                    shot.speed = sim::combat_state::accelerate_projectile(
                        shot.speed, weapon->weapon_velocity, weapon->weapon_acceleration
                    );
                    const bool guided = (flags & OA_WEAPON_FLAG_TWO_PHASE) != 0
                                            ? (shot.flags & OA_PROJECTILE_PHASE_MASK) != 0
                                            : (flags & OA_WEAPON_FLAG_GUIDANCE) != 0;
                    if (guided) {
                        const auto aim =
                            sim::weapon_execution::projectile_aim_point(world, shot, surface, this);
                        const auto desired = sim::weapon_execution::bearing_toward(
                            {shot.position.x, shot.position.y, shot.position.z},
                            {aim.x, aim.y, aim.z}
                        );
                        uint16_t heading = shot.heading;
                        uint16_t pitch = shot.pitch;
                        const bool steered = sim::weapon_execution::steer_projectile(
                            heading, pitch, desired, *weapon
                        );
                        shot.heading = heading;
                        shot.pitch = pitch;
                        if (!steered)
                            detonate(shot, nullptr);
                    }
                    const auto velocity =
                        sim::unit_movement::aim_velocity(shot.heading, shot.pitch, shot.speed);
                    shot.velocity = {velocity[0], velocity[1], velocity[2]};
                } else {
                    shot.velocity.y = wrapping_sub(shot.velocity.y, gravity);
                    shot.pitch = 0;
                }
            } else if ((flags & OA_WEAPON_FLAG_BURN_BLOW) == 0) {
                shot.velocity.y = wrapping_sub(shot.velocity.y, gravity);
                // Two-phase: the first expiry starts the second, guided phase.
                if ((flags & OA_WEAPON_FLAG_TWO_PHASE) != 0 &&
                    (shot.flags & OA_PROJECTILE_PHASE_MASK) == 0) {
                    shot.lifetime_tick =
                        static_cast<uint32_t>(static_cast<uint16_t>(weapon->flight_time)) + tick;
                    shot.flags = static_cast<uint16_t>(
                        (shot.flags & ~OA_PROJECTILE_PHASE_MASK) | OA_PROJECTILE_PHASE_STEP
                    );
                    if ((flags & OA_WEAPON_FLAG_TRACKS) == 0) {
                        shot.intercept_target = 0;
                        shot.target_unit = 0;
                    }
                }
            } else
                detonate(shot, nullptr);
            move();
            break;
        }
        if (collides)
            resolve_projectile_contact(shot);
        // The trail tail runs for every flying record the pass has not retired.
        if ((shot.flags & OA_PROJECTILE_FLAG_RETIRED) == 0)
            projectile_trail_effects(shot, previous_height);
    }
    sim::weapon_execution::compact_projectiles(world);
}

} // namespace oa::sim::match_runtime
