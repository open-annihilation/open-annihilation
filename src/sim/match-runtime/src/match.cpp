// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "match_state.hpp"
#include "enhanced_state.hpp"
#include "oa/data/enhanced_mode.hpp"
#include "oa/sim/ballistics.hpp"
#include "oa/sim/weapon_execution/weapon_launch.hpp"
#include <cstring>
#include <span>
#include <algorithm>
#include <cstdint>
#include "oa/base/game_math.hpp"
#include "oa/sim/world_environment/wind.hpp"
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <memory>

namespace oa::sim::match_runtime {
using data::enhanced_mode::Feature;

namespace {
/// Bounds allocation before the constructor reports invalid match inputs.
///
/// @param input the match inputs
/// @param features the Enhanced Mode features their rules play
/// @return the shot pool's size
std::size_t projectile_pool_size(
    const OfflineInputs& input, data::enhanced_mode::FeatureSet features
) noexcept {
    if (!features.has(Feature::capacity) || input.projectile_capacity < OA_PROJECTILE_CAPACITY ||
        input.projectile_capacity > sim::weapon_execution::maximum_projectile_capacity)
        return OA_PROJECTILE_CAPACITY;
    return input.projectile_capacity;
}

/// Empties the match's shot pool (the "WEAPON ARRAY" block): every record
/// cleared and none in flight.
///
/// @param[in,out] world World whose projectile pool and count are reset.
void clear_projectile_pool(oa::World& world) {
    std::fill_n(world.projectiles, oa::world_projectile_capacity(&world), oa::Projectile{});
    world.game.projectile_count = 0;
}

/// Copies a unit's flags, bucket and attachment links and owner state into
/// its spatial projection; an ownerless unit keeps the owner state it had.
///
/// @param world World holding the unit's owner.
/// @param unit The unit.
/// @param[in,out] projected The unit's spatial projection.
void fill_projection(
    const oa::World& world, const oa::Unit& unit, sim::spatial_state::Unit& projected
) {
    projected.flags = unit.flags;
    projected.next_in_bucket = link_next(unit);
    projected.first_attachment = link_first_child(unit);
    if (const auto* owner = oa::world_unit_owner(&world, &unit)) {
        projected.owner_object_present = owner->in_use != 0;
        projected.owner_status = owner->status;
    }
}

// Sight grid fills: a mapped word holds one bit per player, a coverage word
// counts the stamps that see the cell.
constexpr uint16_t mapped_by_nobody = 0x0000;
constexpr uint16_t mapped_by_all = 0xffff;
constexpr uint16_t unseen = 0;
constexpr uint16_t seen_once = 1;

int16_t low(uint32_t word) {
    return std::bit_cast<int16_t>(static_cast<uint16_t>(word));
}

std::array<int32_t, 3> signed_position(const std::array<uint32_t, 3>& p) {
    return {
        std::bit_cast<int32_t>(p[0]), std::bit_cast<int32_t>(p[1]), std::bit_cast<int32_t>(p[2])
    };
}

struct Geometry final : sim::combat_state::SpawnGeometryHost {
    UnitInstance& unit;

    explicit Geometry(UnitInstance& value) : unit(value) {}

    std::array<int32_t, 3> query_weapon_world(uint8_t slot) override {
        return signed_position(unit.query_weapon_world(slot));
    }

    std::array<int32_t, 3> aim_from_world(uint8_t slot) override {
        return signed_position(unit.aim_from_world(slot));
    }

    void set_max_reload_time(int32_t value) override { unit.set_max_reload_time(value); }
};

struct Activation final : sim::unit_activation::Host {
    sim::unit_spawn::Slot& slot;
    UnitInstance* instance;
    OfflineServices& services;
    std::function<void(uint32_t)> wake_observers; // wakes the orders observing the unit
    sim::simulation_state::World& world;
    const MultiplayerHooks& multiplayer;
    MatchFault& fault;

    Activation(
        sim::unit_spawn::Slot& s,
        UnitInstance* i,
        OfflineServices& h,
        std::function<void(uint32_t)> wake,
        sim::simulation_state::World& w,
        const MultiplayerHooks& m,
        MatchFault& f
    )
        : slot(s), instance(i), services(h), wake_observers(std::move(wake)), world(w),
          multiplayer(m), fault(f) {}

    void script(std::string_view name) override {
        if (instance && instance->script())
            instance->script()->call_no_arguments(name, false);
    }

    void sound(sim::unit_activation::Sound sound) override {
        services.activation_sound(slot, sound);
    }

    void notify_attachments(uint32_t value) override {
        wake_observers(value);
        services.attachment_notification(slot, value);
    }

    void refresh_selected_unit() override { services.refresh_selected_unit(slot); }

    bool owner_simulates_here() override {
        return sim::simulation_state::locally_simulated(*slot.unit);
    }

    void flags_changed(uint16_t unit, uint8_t flags) override {
        if (!world.run_flag)
            return;
        if (!multiplayer.unit_flags_changed) {
            fault.note("unit flag change shared without a multiplayer handler");
            return;
        }
        multiplayer.unit_flags_changed(multiplayer.context, unit, flags);
    }
};

struct MatchWindRandom final : sim::world_environment::WindRandomHost {
    SharedRandom& shared;
    uint32_t& lcg;

    MatchWindRandom(SharedRandom& s, uint32_t& c) : shared(s), lcg(c) {}

    uint32_t lcg_rand_15() override {
        lcg = lcg * 214013u + 2531011u;
        return (lcg >> 16) & 0x7fffu;
    }

    uint32_t shared_random(uint32_t bound) override { return shared.bounded(bound); }
};

struct SpeedScript final : sim::visibility_state::SpeedHost {
    ScriptInstance* script;
    float& storage;
    const float& computed;
    MatchFault& fault;

    SpeedScript(ScriptInstance* value, float& output, const float& input, MatchFault& f)
        : script(value), storage(output), computed(input), fault(f) {}

    void set_speed(int32_t value) override {
        storage = computed;
        if (!script) {
            fault.note("SetSpeed has no script");
            return;
        }
        script->call(ScriptFunction::set_speed, std::span(&value, 1), false);
    }
};

// A record from the runtime type and its parsed definition, for tables built
// without the FBI loader's records.
void assemble_unit_def(const OfflineInputs& input, std::size_t i, oa::UnitDef& def) {
    sim::unit_spawn::load_unit_def(input.types[i], def);
    // The catalog is sorted by unit name, so the index is the type id.
    def.type_id = static_cast<uint16_t>(i);
    const auto& name = input.loaded[i].unit_name;
    const auto length = std::min(name.size(), sizeof def.unit_name - 1);
    std::copy_n(name.data(), length, def.unit_name);
    def.unit_name[length] = '\0';
    def.corpse = input.fields[i].corpse_feature;
    if (const auto* definition = input.fields[i].definition) {
        def.max_velocity = definition->max_velocity_fixed;
        def.move_rate1 = definition->move_rate1_fixed;
        def.move_rate2 = definition->move_rate2_fixed;
        def.sight_distance = definition->sight_distance;
        def.radar_distance = definition->radar_distance;
        def.sonar_distance = definition->sonar_distance;
        def.min_cloak_distance = definition->min_cloak_distance;
        def.radar_distance_jam = definition->radar_distance_jam;
        def.sonar_distance_jam = definition->sonar_distance_jam;
        def.attack_run_length = definition->attack_run_length;
        def.cruise_alt = definition->cruise_altitude;
        def.worker_time = definition->worker_time;
        def.build_distance = definition->build_distance;
        def.maneuver_leash_length = definition->maneuver_leash_length;
        def.build_cost_metal = static_cast<float>(definition->build_cost_metal);
        def.max_water_depth = definition->max_water_depth;
        def.min_water_depth = definition->min_water_depth;
        def.transport_size = definition->transport_size;
        def.transport_capacity = definition->transport_capacity;
        def.kamikaze_distance = definition->kamikaze_distance;
        def.energy_make = definition->energy_make;
        def.energy_use = definition->energy_use;
        def.metal_make = definition->metal_make;
        def.extracts_metal = definition->extracts_metal;
        def.wind_generator = definition->wind_generator;
        def.tidal_generator = definition->tidal_generator;
        def.cloak_cost = definition->cloak_cost;
        def.cloak_cost_moving = definition->cloak_cost_moving;
        def.energy_storage = definition->energy_storage;
        def.metal_storage = definition->metal_storage;
        def.makes_metal = definition->makes_metal;
        def.build_cost_energy = static_cast<float>(definition->build_cost_energy);
        def.build_time = definition->build_time;
        def.acceleration = definition->acceleration_fixed;
        def.brake_rate = definition->brake_rate_fixed;
        def.bank_scale = definition->bank_scale_fixed;
        def.pitch_scale = definition->pitch_scale_fixed;
        def.turn_rate = definition->turn_rate;
    }
}
} // namespace

const char* Match::input_error(const OfflineInputs& input) noexcept {
    if (input.types.empty() || input.loaded.size() != input.types.size() ||
        input.fields.size() != input.types.size() ||
        (!input.unit_defs.empty() && input.unit_defs.size() != input.types.size()))
        return "offline runtime type tables differ";
    if (data::enhanced_mode::features(input.rules).has(Feature::capacity)) {
        if (sim::unit_spawn::unit_pool_size(input.per_player_limit, input.unit_player_mask) == 0)
            return "Enhanced Mode participants exceed the 15-bit unit id capacity";
        if (input.projectile_capacity < OA_PROJECTILE_CAPACITY ||
            input.projectile_capacity > sim::weapon_execution::maximum_projectile_capacity)
            return "Enhanced Mode projectile capacity exceeds the 15-bit projectile id capacity";
    } else if (sim::unit_spawn::unit_pool_size(input.per_player_limit) == 0)
        return "unit pool limit would wrap the 16-bit unit count";
    if (input.viewpoint_player >= 10)
        return "offline viewpoint outside ten players";
    const auto cells =
        static_cast<std::size_t>(input.map.attribute_width) * input.map.attribute_height;
    if (input.terrain_values.size() != cells)
        return "runtime terrain values do not match map";
    if (input.sight_width < 0 || input.sight_height < 0 ||
        static_cast<uint64_t>(input.sight_width) * static_cast<uint32_t>(input.sight_height) >
            16 * 1024 * 1024)
        return "sight grid dimensions exceed bounds";
    if (!input.scenario_definitions)
        return "offline match requires actual scenario definitions";
    if (!input.collision_plots.empty() && input.collision_plots.size() != cells)
        return "collision terrain does not match map";
    if (!input.unit_type_rules.empty() && input.unit_type_rules.size() != input.types.size())
        return "unit type rules do not match the type table";
    return nullptr;
}

Match::State::State(const OfflineInputs& input, data::enhanced_mode::FeatureSet features)
    : world(std::make_unique<oa::World>()),
      units(
          features.has(Feature::capacity)
              ? sim::unit_spawn::unit_pool_size(input.per_player_limit, input.unit_player_mask)
              : sim::unit_spawn::unit_pool_size(input.per_player_limit)
      ),
      unit_defs(input.types.size()), projectiles(projectile_pool_size(input, features)),
      projectile_compaction_storage(
          projectiles.size() > OA_PROJECTILE_CAPACITY
              ? std::make_unique<int16_t[]>(projectiles.size())
              : nullptr
      ),
      projectile_compaction(
          projectile_compaction_storage.get(),
          projectile_compaction_storage ? projectiles.size() : 0
      ),
      assets(units.size()), orders(units.size()) {
    // Refused inputs build the side tables only; the match stops at once.
    if (Match::input_error(input) == nullptr)
        load_types(input);
    world->units = units.data();
    world->unit_slot_count = static_cast<uint32_t>(units.size());
    world->unit_defs = unit_defs.data();
    world->unit_def_count = static_cast<uint32_t>(unit_defs.size());
    feature_defs.assign(input.feature_defs.begin(), input.feature_defs.end());
    world->feature_defs = feature_defs.data();
    world->feature_def_count = static_cast<uint32_t>(feature_defs.size());
    world->game.feature_def_count = static_cast<int32_t>(feature_defs.size());
    plots = std::make_unique<oa::MapPlot[]>(
        static_cast<std::size_t>(input.map.attribute_width) * input.map.attribute_height
    );
    world->plots = plots.get();
    placed_features = std::make_unique<sim::feature_runtime::PlacedFeature[]>(
        sim::feature_runtime::slot_capacity
    );
    world->placed_features = reinterpret_cast<uint8_t*>(placed_features.get());
    world->placed_feature_count = static_cast<uint32_t>(sim::feature_runtime::slot_capacity);
    world->projectiles = projectiles.data();
    world->projectile_capacity = static_cast<uint32_t>(projectiles.size());
    clear_projectile_pool(*world);
    world->game.viewpoint_player = input.viewpoint_player;
    world->game.difficulty = OA_DIFFICULTY_MEDIUM;
    world->game.tidal_strength = input.tidal_strength;
    // Eleven player records, each owning a zeroed setup block; the eleventh
    // keeps the no-player index.
    for (uint32_t i = 0; i < OA_PLAYER_RECORD_COUNT; ++i)
        oa::world_player_record(world.get(), i)->info = oa::oa_ref_from_index(i);
    world->game.no_player.index = OA_PLAYER_COUNT;
    for (std::size_t i = 0; i < OA_PLAYER_COUNT; ++i) {
        auto& player = world->game.players[i];
        player.index = static_cast<uint8_t>(i);
        player.board_row = static_cast<uint8_t>(i); // kills board rows start in player order
        player.economy = oa::oa_ref_from_index(static_cast<uint32_t>(i));
        player.energy_storage = 1000.0F;
        player.metal_storage = 1000.0F;
    }
    if (features.has(Feature::capacity))
        (void)sim::unit_spawn::init_unit_pool(
            *world, input.per_player_limit, input.unit_player_mask
        );
    else
        (void)sim::unit_spawn::init_unit_pool(*world, input.per_player_limit);
    tables = {input.types, assets, setups};
    views =
        std::make_unique<sim::unit_spawn::LegacyViews>(*world, input.types, assets, setups, orders);
}

void Match::State::load_types(const OfflineInputs& input) {
    category_masks.clear();
    const auto category = [&](const data::unit_definitions::UnitCategoryMask& mask) {
        category_masks.push_back(&mask);
        return oa::oa_ref_from_index(static_cast<uint32_t>(category_masks.size() - 1));
    };
    for (std::size_t i = 0; i < input.types.size(); ++i) {
        if (!input.unit_defs.empty())
            unit_defs[i] = input.unit_defs[i];
        else
            assemble_unit_def(input, i, unit_defs[i]);
        // The category handles index this match's mask table.
        if (const auto* masks = input.fields[i].target_masks) {
            unit_defs[i].primary_bad_target_category = category(masks->primary_bad);
            unit_defs[i].secondary_bad_target_category = category(masks->secondary_bad);
            unit_defs[i].special_bad_target_category = category(masks->special_bad);
            unit_defs[i].no_chase_category = category(masks->no_chase);
        }
    }
}

void Match::bind_sides(std::span<const oa::Side> table, uint32_t count) {
    for (uint32_t index = 0; index < OA_SIDE_COUNT; ++index)
        state().game.sides[index] = table[index];
    if (count > OA_SIDE_COUNT) {
        const uint32_t extra = count - OA_SIDE_COUNT;
        state_.extra_sides = std::make_unique<oa::Side[]>(extra);
        for (uint32_t extra_index = 0; extra_index < extra; ++extra_index)
            state_.extra_sides[extra_index] = table[OA_SIDE_COUNT + extra_index];
        state().extra_sides = state_.extra_sides.get();
        state().extra_side_count = extra;
    } else {
        state_.extra_sides.reset();
        state().extra_sides = nullptr;
        state().extra_side_count = 0;
    }
    state().game.side_count = count;
}

void Match::replace_unit_def(std::size_t type, const oa::UnitDef& record) {
    if (type >= state_.unit_defs.size()) {
        fault_.note("unit type outside the runtime table");
        return;
    }
    auto& def = state_.unit_defs[type];
    const auto primary = def.primary_bad_target_category;
    const auto secondary = def.secondary_bad_target_category;
    const auto special = def.special_bad_target_category;
    const auto no_chase = def.no_chase_category;
    def = record;
    def.primary_bad_target_category = primary;
    def.secondary_bad_target_category = secondary;
    def.special_bad_target_category = special;
    def.no_chase_category = no_chase;
}

Match::Match(const OfflineInputs& input, OfflineServices& services)
    : input_(input), enhanced_features_(data::enhanced_mode::features(input.rules)),
      services_(services), state_(input, enhanced_features_),
      simulation_(state_.views->simulation()), world_(state_.views->world()),
      slots_(state_.views->slots()), units_(state_.views->units()), spatial_units_(slots_.size()),
      weapons_(slots_.size()), movement_(slots_.size()), air_drivers_(slots_.size()),
      mirrored_air_goals_(slots_.size()), terrain_(input.map), random_(input.random_seed),
      lcg_seed_(input.random_seed) {
    target_observers_ = std::make_unique<RuntimeOrder*[]>(units_.size());
    if (const auto* refusal = input_error(input)) {
        fault_.note(refusal);
        return;
    }
    keep_rules(input);
    keep_repair_remainders();
    keep_unit_rules();
    if (!enhanced_features_.empty())
        enhanced_ = std::make_unique<EnhancedState>();
    const bool local_flow = enhanced_ && enhanced_features_.has(Feature::local_flow);
    const bool shared_routes = enhanced_ && builds_shared_routes(enhanced_features_);
    if (local_flow) {
        enhanced_->unit_count = slots_.size();
        enhanced_->local_flow_heads =
            std::make_unique<sim::unit_movement::LocalFlowHead[]>(slots_.size());
        enhanced_->local_flow_save_bytes =
            std::make_unique<uint8_t[]>(slots_.size() * sim::unit_movement::local_flow_head_bytes);
        enhanced_->local_flow_progress_bytes = std::make_unique<uint8_t[]>(slots_.size() * 4);
        enhanced_->local_flow_progress_at_tick_start = std::make_unique<uint32_t[]>(slots_.size());
    }
    if (shared_routes)
        enhanced_->static_passability_memo = std::make_unique<StaticPassabilityMemo>();
    if (enhanced_ && indexes_searches(enhanced_features_)) {
        enhanced_->order_ownership = std::make_unique<OrderOwnershipIndex>(slots_.size());
        constexpr size_t orders_per_unit = 4;
        orders_.reserve(slots_.size() * orders_per_unit);
        enhanced_->target_query_slots = std::make_unique<uint16_t[]>(2 * slots_.size());
        enhanced_->target_query_extras = std::make_unique<uint16_t[]>(slots_.size());
        enhanced_->target_sighting_membership =
            std::make_unique<uint8_t[]>(slots_.size() * OA_PLAYER_COUNT);
        sighting_query_.reserve(slots_.size());
        for (size_t player = 0; player < seen_before_.size(); ++player) {
            seen_before_[player] = std::make_unique<sim::detection::SightedUnit[]>(slots_.size());
            seen_before_room_[player] = static_cast<uint32_t>(slots_.size());
        }
        seen_work_ = std::make_unique<sim::detection::SightedUnit[]>(3 * slots_.size());
        seen_work_room_ = static_cast<uint32_t>(slots_.size());
    }
    target_projection_.resize(slots_.size());
    const auto sighting_capacity = static_cast<uint32_t>(slots_.size());
    // Each player's seen, radar and pad lists.
    constexpr std::size_t lists = 3;
    sighting_slots_.assign(lists * sightings_.size() * sighting_capacity, 0);
    for (std::size_t player = 0; player < sightings_.size(); ++player) {
        auto& sightings = sightings_[player];
        sightings.seen = &sighting_slots_[lists * player * sighting_capacity];
        sightings.radar = sightings.seen + sighting_capacity;
        sightings.pads = sightings.radar + sighting_capacity;
        sightings.capacity = sighting_capacity;
    }
    type_bounds_.resize(input.types.size());
    for (std::size_t i = 1; i < input.types.size(); ++i) {
        if (!input.loaded[i].model)
            continue;
        const auto derived = formats::objects3d::derive_unit_type_bounds(
            *input.loaded[i].model, input.types[i].footprint_x, input.types[i].footprint_z
        );
        // Only a model whose links are broken has no height, and a loaded
        // model never has: its type keeps the bounds it has.
        if (!derived.ok())
            continue;
        const auto& bounds = type_bounds_[i] = *derived.value;
        auto& def = state_.unit_defs[i];
        def.bounds_min_x = bounds->bounds_min_x;
        def.bounds_min_y = bounds->bounds_min_y;
        def.bounds_min_z = bounds->bounds_min_z;
        def.bounds_max_x = bounds->bounds_max_x;
        def.model_height = bounds->model_height;
        def.bounds_max_z = bounds->bounds_max_z;
        def.size_x = bounds->size_x;
        def.size_y = bounds->size_y;
        def.size_z = bounds->size_z;
        def.size_radius =
            std::bit_cast<int32_t>(
                std::bit_cast<uint32_t>(bounds->size_z) + std::bit_cast<uint32_t>(bounds->size_x)
            ) /
            3;
    }
    const auto cells =
        static_cast<std::size_t>(input.map.attribute_width) * input.map.attribute_height;
    sim::scenario::construct(scenario_);
    if (const auto refused =
            sim::scenario::register_conditions(scenario_, *input.scenario_definitions);
        refused != sim::scenario::DefinitionError::none)
        fault_.note(sim::scenario::definition_error_text(refused));
    scenario_gravity_ = input.scenario_definitions->integer("gravity", 0);
    lava_world_ = input.scenario_definitions->integer("lavaworld", 0);
    // The schema's waterdoesdamage and waterdamage, which the unit tick
    // applies to units at or below sea level.
    state().environment_enabled =
        static_cast<uint32_t>(input.scenario_definitions->integer("waterdoesdamage", 0));
    state().environment_damage = input.scenario_definitions->integer("waterdamage", 0);
    state().game.gravity = sim::ballistics::simulation_gravity(false, 0, scenario_gravity_);
    const auto weapon_records = input.weapons.records();
    const std::size_t inline_slots =
        std::min(weapon_records.size(), static_cast<std::size_t>(OA_WEAPON_DEF_COUNT));
    std::copy_n(weapon_records.begin(), inline_slots, std::begin(state().game.weapon_defs));
    if (weapon_records.size() > OA_WEAPON_DEF_COUNT) {
        const std::size_t extra = weapon_records.size() - OA_WEAPON_DEF_COUNT;
        state_.extra_weapon_defs = std::make_unique<oa::WeaponDef[]>(extra);
        std::copy_n(
            weapon_records.begin() + static_cast<std::ptrdiff_t>(OA_WEAPON_DEF_COUNT),
            extra,
            state_.extra_weapon_defs.get()
        );
        state().extra_weapon_defs = state_.extra_weapon_defs.get();
        state().extra_weapon_def_count = static_cast<uint32_t>(extra);
    }
    state().game.sea_level = terrain_.sea_level();
    // The map's cell counts and its size in world units.
    state().game.map_width = static_cast<int32_t>(input.map.attribute_width);
    state().game.map_height = static_cast<int32_t>(input.map.attribute_height);
    state().game.map_width_world = state().game.map_width << 4;
    state().game.map_height_world = state().game.map_height << 4;
    // The session's mapping and line-of-sight rules; the fog edge mask
    // starts stale.
    state().game.visibility_flags =
        static_cast<uint8_t>(input.visibility_flags & ~OA_VISIBILITY_FOG_MASK_CURRENT);
    spatial_.terrain_width = input.map.attribute_width;
    spatial_.terrain_height = input.map.attribute_height;
    if (shared_routes) {
        constexpr uint32_t fine_flow_field_limit = 80;
        constexpr uint32_t fine_flow_cell_limit = 8 * 1024 * 1024;
        // Movement class and player pairs whose landmasses are labelled, so
        // that a goal a unit cannot reach is replaced by the nearest point it
        // can.
        constexpr uint32_t fine_flow_landmass_slots = 8;
        auto flow_limits = sim::ground_orders::FlowLimits{};
        flow_limits.fields = fine_flow_field_limit;
        flow_limits.total_cells = fine_flow_cell_limit;
        flow_limits.landmass_slots = fine_flow_landmass_slots;
        enhanced_->shared_routes = std::make_unique<sim::ground_orders::FlowCache>(
            spatial_.terrain_width, spatial_.terrain_height, flow_limits
        );
    }
    // A save restores these tables by name in the order they were added:
    // keep this order.
    const auto add_navigation_table = [this](bool used, const RuleStateTable& table) {
        return !used || add_rule_state(rule_state_, table);
    };
    const bool routes_added = add_navigation_table(
        shared_routes,
        {"shared-routes",
         enhanced_.get(),
         [](void* context) {
             return static_cast<EnhancedState*>(context)->shared_routes->state_bytes();
         },
         [](void* context, std::span<const uint8_t> bytes) {
             auto& state = *static_cast<EnhancedState*>(context);
             if (!state.shared_routes->restore(bytes))
                 return false;
             return true;
         },
         false}
    );
    const bool heads_added = add_navigation_table(
        local_flow,
        {"local-flow",
         enhanced_.get(),
         [](void* context) -> std::span<const uint8_t> {
             auto& state = *static_cast<EnhancedState*>(context);
             std::span<uint8_t> bytes{
                 state.local_flow_save_bytes.get(),
                 state.unit_count * sim::unit_movement::local_flow_head_bytes
             };
             if (!sim::unit_movement::write_local_flow_heads(
                     {state.local_flow_heads.get(), state.unit_count}, bytes
                 ))
                 return {};
             return bytes;
         },
         [](void* context, std::span<const uint8_t> bytes) {
             auto& state = *static_cast<EnhancedState*>(context);
             return sim::unit_movement::read_local_flow_heads(
                 {state.local_flow_heads.get(), state.unit_count}, bytes
             );
         },
         false}
    );
    const bool revision_added = add_navigation_table(
        shared_routes,
        {"flow-map-version",
         enhanced_.get(),
         [](void* context) -> std::span<const uint8_t> {
             auto& state = *static_cast<EnhancedState*>(context);
             for (size_t byte = 0; byte < state.shared_static_revision_bytes.size(); ++byte)
                 state.shared_static_revision_bytes[byte] =
                     static_cast<uint8_t>(state.shared_static_revision >> (byte * 8));
             return state.shared_static_revision_bytes;
         },
         [](void* context, std::span<const uint8_t> bytes) {
             if (bytes.size() != 4)
                 return false;
             auto& state = *static_cast<EnhancedState*>(context);
             state.shared_static_revision = 0;
             for (size_t byte = 0; byte < bytes.size(); ++byte)
                 state.shared_static_revision |= static_cast<uint32_t>(bytes[byte]) << (byte * 8);
             return true;
         },
         false}
    );
    const bool progress_added = add_navigation_table(
        local_flow,
        {"flow-progress",
         this,
         [](void* context) -> std::span<const uint8_t> {
             auto& match = *static_cast<Match*>(context);
             for (size_t id = 0; id < match.enhanced_->unit_count; ++id) {
                 const auto* ground = match.ground_runtime(static_cast<uint16_t>(id));
                 const auto tick = ground != nullptr ? ground->movement.last_motion_tick : 0;
                 for (size_t byte = 0; byte < 4; ++byte)
                     match.enhanced_->local_flow_progress_bytes[id * 4 + byte] =
                         static_cast<uint8_t>(tick >> (byte * 8));
             }
             return {
                 match.enhanced_->local_flow_progress_bytes.get(), match.enhanced_->unit_count * 4
             };
         },
         [](void* context, std::span<const uint8_t> bytes) {
             auto& match = *static_cast<Match*>(context);
             if (bytes.size() != match.enhanced_->unit_count * 4)
                 return false;
             for (size_t id = 0; id < match.enhanced_->unit_count; ++id) {
                 if (auto* ground = match.ground_runtime(static_cast<uint16_t>(id))) {
                     uint32_t tick = 0;
                     for (size_t byte = 0; byte < 4; ++byte)
                         tick |= static_cast<uint32_t>(bytes[id * 4 + byte]) << (byte * 8);
                     ground->movement.last_motion_tick = tick;
                 }
             }
             return true;
         },
         false}
    );
    const bool scan_added = add_navigation_table(
        shared_routes,
        {"shared-route-scan",
         enhanced_.get(),
         [](void* context) -> std::span<const uint8_t> {
             return static_cast<EnhancedState*>(context)->shared_route_scan_state;
         },
         [](void* context, std::span<const uint8_t> bytes) {
             auto& state = *static_cast<EnhancedState*>(context);
             if (bytes.size() != state.shared_route_scan_state.size() || bytes[0] > 1 ||
                 (bytes[0] == 0 && std::any_of(bytes.begin() + 1, bytes.end(), [](uint8_t byte) {
                      return byte != 0;
                  })))
                 return false;
             std::copy(bytes.begin(), bytes.end(), state.shared_route_scan_state.begin());
             return true;
         },
         false}
    );
    if (!routes_added || !heads_added || !revision_added || !progress_added || !scan_added)
        fault_.note("Enhanced navigation state could not be registered");
    if (enhanced_ && enhanced_features_.has(Feature::ai_strike_retarget)) {
        const bool retarget_added = add_rule_state(
            rule_state_,
            {"ai-retarget",
             this,
             [](void* context) -> std::span<const uint8_t> {
                 auto& match = *static_cast<Match*>(context);
                 return match.enhanced_->ai_retarget.state_bytes(match.slots_.size());
             },
             [](void* context, std::span<const uint8_t> bytes) {
                 auto& match = *static_cast<Match*>(context);
                 return match.enhanced_->ai_retarget.restore(bytes, match.slots_.size());
             },
             false}
        );
        if (!retarget_added)
            fault_.note("Enhanced strike retarget state could not be registered");
    }
    spatial_.plots.resize(cells);
    spatial_.units = spatial_units_;
    spatial_.sea_level = state().game.sea_level;
    collision_terrain_ = !input.collision_plots.empty();
    // The plots are copied here; input_ keeps no reference to them.
    input_.collision_plots = {};
    if (collision_terrain_) {
        for (std::size_t i = 0; i < cells; ++i) {
            spatial_.plots[i].high_height = input.collision_plots[i].high_height;
            spatial_.plots[i].low_height = input.collision_plots[i].low_height;
            spatial_.plots[i].blocking_feature = input.collision_plots[i].blocking_feature;
            spatial_.plots[i].metal_feature = input.collision_plots[i].metal_feature;
            spatial_.plots[i].geo_feature = input.collision_plots[i].geo_feature;
            spatial_.plots[i].indestructible_feature =
                input.collision_plots[i].indestructible_feature;
            spatial_.plots[i].metal = input.collision_plots[i].metal;
            spatial_.plots[i].feature_word = input.collision_plots[i].feature_word;
            spatial_.plots[i].feature_back_x = input.collision_plots[i].feature_back_x;
            spatial_.plots[i].feature_back_z = input.collision_plots[i].feature_back_z;
            spatial_.plots[i].feature_height = input.collision_plots[i].feature_height;
            spatial_.plots[i].feature_footprint_x = input.collision_plots[i].feature_footprint_x;
            spatial_.plots[i].feature_footprint_z = input.collision_plots[i].feature_footprint_z;
        }
    }
    // The plot metal is copied here too, for the features to paint; input_
    // keeps no reference to it.
    plot_metal_ = std::make_unique<sim::visibility_state::TerrainCell[]>(cells);
    std::copy(input.terrain_values.begin(), input.terrain_values.end(), plot_metal_.get());
    input_.terrain_values = {};
    sim::spatial_state::build_buckets(spatial_);
    sight_.width = input.sight_width;
    sight_.height = input.sight_height;
    sight_.viewpoint_player = input.viewpoint_player;
    sight_.game = &state().game;
    const auto sight_cells =
        static_cast<std::size_t>(sight_.width) * static_cast<std::size_t>(sight_.height);
    sight_.coverage.resize(sight_cells);
    sight_.player_bits.resize(sight_cells);
    for (std::size_t i = 0; i < other_player_coverage_.size(); ++i)
        if (i != input.viewpoint_player)
            other_player_coverage_[i].resize(sight_cells);
    // Each player's coverage grid is sight_width by sight_height cells.
    for (auto& player : state().game.players) {
        player.sight_width = static_cast<uint32_t>(sight_.width);
        player.sight_height = static_cast<uint32_t>(sight_.height);
    }
    if (const auto* error = sim::visibility_state::sight_context_error(sight_context())) {
        fault_.note(error);
        return;
    }
    bridge_ = std::make_unique<SpawnBridge>(
        *state_.world,
        state_.tables,
        *state_.views,
        input.loaded,
        static_cast<UnitValueHost&>(*this),
        services_,
        static_cast<SpawnSubsystems&>(*this),
        random_,
        input.clock_scale,
        fault_
    );
    environment_wind_.minimum_strength = input.minimum_wind;
    environment_wind_.maximum_strength = input.maximum_wind;
    if (environment_wind_.maximum_strength < environment_wind_.minimum_strength)
        std::swap(environment_wind_.maximum_strength, environment_wind_.minimum_strength);
    environment_wind_.current_tick = 0;
    // In 3.1c the wind schedule of a session's later matches draws from the
    // LCG stream, since the tick counter still holds the last match's
    // count when the wind is scheduled; a match here always starts at tick 0
    // and draws nothing.
    keep_wind_generator(input.rules.economy.deterministic_wind);
    MatchWindRandom wind_random{random_, lcg_seed_};
    if (wind_generator_)
        note_wind(
            sim::world_environment::initialize_shared_wind(
                environment_wind_, *wind_generator_, shared_wind_seed()
            )
        );
    else
        note_wind(sim::world_environment::initialize_wind(environment_wind_, wind_random));
    effects_ = std::make_unique<sim::effect_particles::EffectWorld>();
    if (!sim::effect_particles::size_effect_world(*effects_, input.limits.effects))
        fault_.note("the effect layers cannot be allocated");
    search_.jobs.tick_credit = input.limits.path_search.nodes;
    effects_->lava_world = lava_world_ != 0;
    effects_->explosion_smoke_column = input.display.explosion_smoke_column;
    effects_->no_sea_level_trigger =
        input.scenario_definitions->integer("nosealeveltrigger", 0) != 0;
    allocate_flash_tiers();
    sim::effect_particles::init_explosion_tables(*effects_, flash_tiers_, effect_host());
    resolve_effect_sequences();
    place_map_features();
    wind_.changed = environment_wind_.changed != 0;
    wind_.direction = environment_wind_.direction;
    wind_.speed = static_cast<uint32_t>(environment_wind_.strength);
    configure_strategic_environment(
        {environment_wind_.maximum_strength,
         input.tidal_strength,
         environment_wind_.normalized_strength}
    );
    build_movement_maps();
    add_allied_sight_state();
}

Match::~Match() = default;

void Match::invalidate_target_query_extras() noexcept {
    if (enhanced_ && indexes_searches(enhanced_features_))
        enhanced_->target_query_extras_valid = false;
}

EnhancedCounters Match::enhanced_counters() const noexcept {
    if (!enhanced_)
        return {};
    auto result = enhanced_->counters;
    const auto& local = enhanced_->local_flow_counters;
    result.local_flow_probes = local.probes;
    result.moving_blockers = local.moving_blockers;
    result.steering_adjustments = local.steering_adjustments;
    result.yielded_ticks = local.yielded_ticks;
    result.blocked_ticks = local.blocked_ticks;
    result.deferred_replans = local.deferred_replans;
    if (enhanced_->shared_routes) {
        const auto shared = enhanced_->shared_routes->counters();
        result.shared_route_requests = shared.requests;
        result.shared_route_hits = shared.hits;
        result.shared_route_fields = shared.fields_started;
        result.shared_route_cells = shared.cells_cleared + shared.cells_expanded;
        result.shared_route_waits = shared.waits;
        result.shared_route_invalidations = shared.invalidations;
    }
    return result;
}

const sim::unit_movement::LocalFlowHead* Match::local_flow_head(uint16_t unit) const noexcept {
    if (!enhanced_ || !enhanced_features_.has(Feature::local_flow) || unit >= enhanced_->unit_count)
        return nullptr;
    return &enhanced_->local_flow_heads[unit];
}

void Match::note_wind(sim::world_environment::WindRefresh result) noexcept {
    switch (result) {
    case sim::world_environment::WindRefresh::random_out_of_range:
        fault_.note("wind rand() value above 32767");
        break;
    case sim::world_environment::WindRefresh::zero_strength_divisor:
        fault_.note("wind-strength divisor is zero");
        break;
    case sim::world_environment::WindRefresh::waiting:
    case sim::world_environment::WindRefresh::changed:
        break;
    }
}

const RuntimeTypeFields& Match::fields(sim::unit_spawn::Slot& slot) const {
    if (slot.unit)
        if (const auto entry = type_entry(input_.types, *slot.unit)) {
            if (input_.fields[*entry].definition)
                return input_.fields[*entry];
            fault_.note("runtime type has no FBI definition");
            return unresolved_fields();
        }
    fault_.note("unit type outside runtime table");
    return unresolved_fields();
}

std::optional<std::size_t> Match::type_entry(
    std::span<const sim::unit_spawn::Type> types, const sim::simulation_state::Unit& unit
) {
    const sim::simulation_state::UnitType* type = unit.type;
    if (type == nullptr)
        return std::nullopt;
    if (const auto ref = unit.record.def;
        ref != 0 && ref <= types.size() && &types[ref - 1u].simulation == type)
        return ref - 1u;
    for (std::size_t i = 0; i < types.size(); ++i)
        if (&types[i].simulation == type)
            return i;
    return std::nullopt;
}

const RuntimeTypeFields& Match::unresolved_fields() noexcept {
    static const data::unit_definitions::UnitDefinition definition{};
    static const data::unit_definitions::RuntimeDefinitionMetadata metadata{};
    static const data::unit_definitions::UnitTargetCategoryMasks masks{};
    static const RuntimeTypeFields fields{&definition, {}, std::nullopt, &metadata, &masks};
    return fields;
}

sim::unit_spawn::Slot* Match::create(const sim::unit_spawn::Request& request, uint16_t builder) {
    invalidate_target_query_extras();
    auto* slot = bridge_->create(request);
    report_created(slot, request.finished, builder);
    return slot;
}

sim::unit_spawn::StartResult Match::start_player(
    uint8_t player,
    const sim::unit_spawn::PlayerSetup& setup,
    std::span<const sim::unit_spawn::StartMarker> markers,
    int32_t index,
    int32_t width,
    int32_t height,
    sim::unit_spawn::StartHost& host
) {
    invalidate_target_query_extras();
    const auto started = bridge_->start_player(
        player, setup, markers, index, input_.viewpoint_player, width, height, host
    );
    // The commander is created finished, and no unit built it.
    if (started.unit != nullptr)
        report_created(&slots_.at(oa::world_unit_slot(&state(), started.unit)), true, 0);
    return started;
}

EventListenerId Match::add_event_listener(const EventHooks& hooks) noexcept {
    if (event_listener_count_ >= max_event_listeners)
        return 0;
    if (next_event_listener_id_ == 0)
        next_event_listener_id_ = 1;
    const auto id = next_event_listener_id_;
    ++next_event_listener_id_;
    if (next_event_listener_id_ == 0)
        next_event_listener_id_ = 1;
    event_listeners_[event_listener_count_] = EventListener{id, hooks};
    ++event_listener_count_;
    return id;
}

bool Match::remove_event_listener(EventListenerId id) noexcept {
    if (id == 0)
        return false;
    for (std::size_t listener_index = 0; listener_index < event_listener_count_; ++listener_index) {
        if (event_listeners_[listener_index].id != id)
            continue;
        for (std::size_t later = listener_index + 1; later < event_listener_count_; ++later)
            event_listeners_[later - 1] = event_listeners_[later];
        --event_listener_count_;
        event_listeners_[event_listener_count_] = {};
        return true;
    }
    return false;
}

std::size_t Match::event_listener_count() const noexcept {
    return event_listener_count_;
}

void Match::report_created(const sim::unit_spawn::Slot* slot, bool finished, uint16_t builder) {
    if (enhanced_ && slot != nullptr && slot->unit_index < enhanced_->unit_count) {
        enhanced_->local_flow_heads[slot->unit_index] = {};
        const auto* ground = ground_runtime(slot->unit_index);
        enhanced_->local_flow_progress_at_tick_start[slot->unit_index] =
            ground != nullptr ? ground->movement.last_motion_tick : simulation_.tick;
    }
    if (slot == nullptr || slot->unit == nullptr)
        return;
    const auto unit = slot->unit_index;
    // A kept picture starts the new slot as a new unit's flags start, with
    // that player as the viewer. This does not wait on an event hook.
    if (player_contacts_ != nullptr) {
        const auto stride = slots_.size();
        if (unit < stride) {
            const auto& created = state().units[unit];
            for (uint8_t picture = 0; picture < OA_PLAYER_COUNT; ++picture) {
                if (player_contacts_kept_[picture] == 0)
                    continue;
                player_contacts_[static_cast<std::size_t>(picture) * stride + unit] =
                    sim::detection::created_picture_bits(created, picture);
            }
        }
    }
    bridge_->runtime(slots_.at(unit)).finished_reported = false;
    const auto& world = state();
    emit_event<&EventHooks::unit_created>(world, unit);
    emit_event<&EventHooks::unit_created_by>(world, unit, builder);
    const auto type = world.units[unit].type_index;
    if (finished && type < world.unit_def_count && world.unit_defs[type].bm_code == 0)
        report_finished(unit, unit);
}

void Match::report_finished(uint16_t unit, uint16_t builder) {
    auto& reported = bridge_->runtime(slots_.at(unit)).finished_reported;
    if (reported)
        return;
    reported = true;
    emit_event<&EventHooks::unit_finished>(state(), unit, builder);
}

UnitInstance* Match::instance(uint16_t index) {
    return bridge_->runtime(slots_.at(index)).instance.get();
}

void Match::share_script_start(uint16_t unit, int16_t function) {
    if (multiplayer.script_started != nullptr)
        multiplayer.script_started(multiplayer.context, unit, function, 0, {});
}

void Match::share_named_script_start(
    uint16_t unit, std::string_view name, uint8_t count, const std::array<uint32_t, 4>& locals
) {
    if (multiplayer.script_started == nullptr)
        return;
    auto* object = instance(unit);
    const auto function =
        object != nullptr && object->script() != nullptr ? object->script()->find(name) : -1;
    multiplayer.script_started(
        multiplayer.context, unit, static_cast<int16_t>(function), count, locals
    );
}

void Match::tick_scripts(uint32_t elapsed) {
    for (auto& slot : slots_)
        if (slot.unit->record.type_index && slot.unit->script_present)
            bridge_->tick_script(slot, elapsed);
}

namespace {
// The bytes of the shared wind generator, as its rule-state table keeps them.
std::span<const uint8_t> wind_generator_bytes(void* context) {
    const auto* generator = static_cast<const sim::world_environment::WindGenerator*>(context);
    return {reinterpret_cast<const uint8_t*>(generator), sizeof *generator};
}

// Restores the shared wind generator from a save's table; false when the
// bytes are not one generator in range.
bool restore_wind_generator(void* context, std::span<const uint8_t> bytes) {
    sim::world_environment::WindGenerator saved{};
    if (bytes.size() != sizeof saved)
        return false;
    std::memcpy(&saved, bytes.data(), sizeof saved);
    if (!sim::world_environment::wind_generator_valid(saved))
        return false;
    *static_cast<sim::world_environment::WindGenerator*>(context) = saved;
    return true;
}
} // namespace

void Match::keep_wind_generator(const data::match_rules::EconomyDeterministicWind& rule) {
    if (!rule.enabled ||
        rule.rng != data::match_rules::EconomyDeterministicWindRng::mt19937_host_id)
        return;
    wind_generator_ = std::make_unique<sim::world_environment::WindGenerator>();
    if (!add_rule_state(
            rule_state_,
            {"wind-generator", wind_generator_.get(), wind_generator_bytes, restore_wind_generator}
        ))
        fault_.note("the wind generator's rule state cannot be kept");
}

uint32_t Match::shared_wind_seed() const {
    // A seeded generator ignores the seed; only the first change looks the
    // host up.
    if (wind_generator_->seeded != 0)
        return 0;
    return sim::world_environment::shared_wind_seed(state(), input_.random_seed);
}

void Match::refresh_wind() {
    environment_wind_.current_tick = simulation_.tick;
    MatchWindRandom wind_random{random_, lcg_seed_};
    if (wind_generator_)
        note_wind(
            sim::world_environment::refresh_shared_wind(
                environment_wind_, *wind_generator_, shared_wind_seed()
            )
        );
    else
        note_wind(sim::world_environment::refresh_wind(environment_wind_, wind_random));
    sim::world_environment::store_wind_state(state().game, environment_wind_);
    wind_.changed = environment_wind_.changed != 0;
    wind_.direction = environment_wind_.direction;
    wind_.speed = static_cast<uint32_t>(environment_wind_.strength);
}

void Match::tick(sim::simulation_state::Host& host) {
    if (!outcome_view_) {
        fault_.note("offline tick requires configured outcome identity and alliances");
        return;
    }
    resolve_effect_sequences();
    if (enhanced_ && enhanced_->local_flow_progress_at_tick_start) {
        for (size_t id = 0; id < enhanced_->unit_count; ++id) {
            const auto* ground = ground_runtime(static_cast<uint16_t>(id));
            enhanced_->local_flow_progress_at_tick_start[id] =
                ground != nullptr ? ground->movement.last_motion_tick : 0;
        }
        enhanced_->local_flow_progress_snapshot_tick = simulation_.tick;
        enhanced_->local_flow_progress_snapshot_valid = true;
    }
    // The game loop's order: units and their scripts, projectiles, then
    // explosions, so records a script or an impact logs are stepped in the
    // tick that logs them.
    note_step(sim::simulation_state::update_units(state(), state_.orders, host));
    mark_profile(OA_PROFILE_UNITS);
    update_projectiles();
    mark_profile(OA_PROFILE_WEAPON);
    sim::effect_particles::tick_explosions(*effects_, state().game, effect_host());
    mark_profile(OA_PROFILE_MISC);
    advance_path_search();
}

void Match::target_cleared(oa::Unit& unit, uint8_t slot) {
    auto* object = instance(unit.id);
    if (object && object->script()) {
        const int32_t argument = slot;
        object->script()->call(ScriptFunction::target_cleared, std::span(&argument, 1), false);
    }
}

sim::unit_health::ParalysisHooks Match::weapon_target_hooks() {
    sim::unit_health::ParalysisHooks hooks;
    hooks.context = this;
    hooks.target_cleared = [](void* context, oa::Unit& unit, uint8_t slot) {
        static_cast<Match*>(context)->target_cleared(unit, slot);
    };
    return hooks;
}

void Match::note_step(sim::simulation_state::StepFault fault) noexcept {
    using sim::simulation_state::StepFault;
    switch (fault) {
    case StepFault::none:
        break;
    case StepFault::order_budget_spent:
        fault_.note("order execution budget exhausted; unresolved handler failed to yield");
        break;
    case StepFault::order_not_queued:
        fault_.note("rotated order is not in primary queue");
        break;
    case StepFault::untyped_unit:
        fault_.note("unit has no type");
        break;
    case StepFault::orders_short:
        fault_.note("order queues do not cover the unit pool");
        break;
    }
}

void Match::stop_weapon(sim::unit_spawn::Slot& slot, uint32_t index) {
    if (index >= OA_UNIT_WEAPON_COUNT) {
        fault_.note("weapon slot outside the unit");
        return;
    }
    (void)sim::unit_health::clear_weapon_target(
        slot.record, static_cast<uint8_t>(index), weapon_target_hooks()
    );
}

void Match::release_tracked_weapons(sim::unit_spawn::Slot& slot, uint8_t index) {
    (void)sim::unit_health::clear_tracked_weapon_targets(slot.record, index, weapon_target_hooks());
}

void Match::assign_squad(sim::unit_spawn::Slot& slot, uint32_t group) {
    if (!slot.unit || !slot.unit->owner) {
        fault_.note("unit group owner");
        return;
    }
    const auto owner = slot.unit->owner->record.index;
    if (owner >= groups_.size()) {
        fault_.note("unit group owner");
        return;
    }
    auto& groups = groups_[owner];
    if (slot.record.squad != -1) {
        auto& old = groups[static_cast<uint32_t>(slot.record.squad)];
        const auto found = std::find(old.begin(), old.end(), slot.unit_index);
        if (found != old.end()) {
            *found = old.back();
            old.pop_back();
        }
    }
    if (group != 0xffffffffu)
        groups[group].push_back(slot.unit_index);
    slot.record.squad = static_cast<int32_t>(group);
}

void Match::initialize_weapons(sim::unit_spawn::Slot& slot, SlotRuntime& runtime) {
    if (!runtime.instance) {
        fault_.note("combat initialization precedes model");
        return;
    }
    const auto& def = *fields(slot).definition;
    Geometry geometry(*runtime.instance);
    auto& weapons = weapons_.at(slot.unit_index);
    for (std::size_t i = 0; i < weapons.slots.size(); ++i)
        weapons.slots[i].flags = slot.record.weapons[i].flags;
    if (sim::combat_state::initialize_spawn_combat(
            weapons, input_.weapons, {def.weapon1, def.weapon2, def.weapon3}, geometry
        )
            .definition_cleared) {
        fault_.note("geometry callback cleared weapon definition");
        return;
    }
    for (std::size_t i = 0; i < weapons.slots.size(); ++i)
        sim::weapon_execution::arm_weapon_slot(
            slot.record.weapons[i], weapons.slots[i], weapons.definitions[i]
        );
}

void Match::initialize_extraction_rate(sim::unit_spawn::Slot& slot, SlotRuntime& runtime) {
    const auto& def = *fields(slot).definition;
    sim::visibility_state::SpeedUnit unit{
        def.extracts_metal,
        slot.record.cell_x,
        slot.record.cell_z,
        slot.record.footprint_x,
        slot.record.footprint_z,
        slot.record.extracted_metal,
        slot.record.script != 0
    };
    SpeedScript script(
        runtime.instance ? runtime.instance->script() : nullptr,
        slot.record.extracted_metal,
        unit.speed,
        fault_
    );
    const auto width = static_cast<int32_t>(input_.map.attribute_width);
    const auto height = static_cast<int32_t>(input_.map.attribute_height);
    const std::span<const sim::visibility_state::TerrainCell> metal(
        plot_metal_.get(),
        plot_metal_ ? static_cast<std::size_t>(width) * static_cast<std::size_t>(height) : 0
    );
    (void)sim::visibility_state::initialize_terrain_speed(
        unit, {width, height, metal}, unit.script_present ? &script : nullptr
    );
    slot.record.extracted_metal = unit.speed;
}

sim::unit_spawn::AssetHandle Match::create_movement(sim::unit_spawn::Slot& slot) {
    const auto& type = fields(slot);
    if (!type.movement_class) {
        fault_.note("movement class pointer has not been resolved");
        return 0;
    }
    if (!slot.unit || !slot.unit->type) {
        fault_.note("Ground movement requires a bound unit and type");
        return 0;
    }
    auto& runtime = movement_.at(slot.unit_index);
    runtime = std::make_unique<sim::ground_orders::GroundRuntime>(
        slot, *type.definition, *type.movement_class
    );
    // A can-fly type's movement object gets the air driver: local, or a
    // mirrored copy for units another player's simulation runs.
    auto& driver = air_drivers_.at(slot.unit_index);
    driver = {};
    mirrored_air_goals_.at(slot.unit_index) = {};
    if (slot.unit->type && (slot.unit->type->flags & OA_UNIT_DEF_FLAG_CAN_FLY) != 0) {
        if (enhanced_ && indexes_searches(enhanced_features_))
            enhanced_->air_goal_cleanup_required = true;
        if (runtime->mirrored_driver)
            sim::air::air_driver_init_mirrored(&driver, &slot.record);
        else
            sim::air::air_driver_init_local(&driver, &slot.record);
    }
    return reinterpret_cast<sim::unit_spawn::AssetHandle>(runtime.get());
}

void Match::fit_spawn_height(sim::unit_spawn::Slot& slot) {
    auto& unit = *slot.unit;
    const auto flags = unit.flags;
    const auto& type = *unit.type;
    if (!(flags & OA_UNIT_FLAG_POSITION_DIRTY) && !(type.flags & OA_UNIT_DEF_FLAG_CAN_HOVER))
        return;
    unit.flags &= ~OA_UNIT_FLAG_POSITION_DIRTY;
    if (!unit.object_present || (flags & OA_UNIT_FLAG_OCCUPANCY_MASK) != 1)
        return;
    if (type.flags & OA_UNIT_DEF_FLAG_UPRIGHT) {
        int32_t height;
        if (!(type.flags & OA_UNIT_DEF_FLAG_CAN_HOVER))
            height = sample_terrain_height(unit.position[0], unit.position[2]);
        else {
            height = static_cast<int32_t>(simulation_.sea_level) - type.waterline_offset;
            if (height < sample_terrain_height(unit.position[0], unit.position[2]))
                height = sample_terrain_height(unit.position[0], unit.position[2]);
        }
        unit.position[1] = static_cast<uint32_t>(height) << 16;
    } else if (type.flags & OA_UNIT_DEF_FLAG_FLOATER)
        unit.position[1] =
            (static_cast<uint32_t>(type.waterline_offset) * 0xffffu + simulation_.sea_level) << 16;
    else {
        // Every other type, aircraft included, takes the ground fit over its
        // model's ground plate; nothing lifts a hovercraft the fit leaves
        // under the surface back to it.
        auto& runtime = movement_.at(slot.unit_index);
        if (!runtime) {
            fault_.note("ground fit has no movement object");
            return;
        }
        auto* object = instance(slot.unit_index);
        if (!object) {
            fault_.note("ground fit has no model object");
            return;
        }
        sim::unit_movement::GroundClock clock;
        clock.simulation_tick = simulation_.tick;
        if (type.flags & OA_UNIT_DEF_FLAG_CAN_HOVER) {
            if (!input_.uptime_milliseconds) {
                fault_.note("floating ground fit requires platform clock");
                return;
            }
            for (auto& tick : clock.bob_ticks)
                tick = sim::unit_movement::scaled_bob_tick(
                    input_.uptime_milliseconds(), static_cast<uint32_t>(input_.clock_scale)
                );
        }
        (void)runtime->fit_height(terrain_, object->model().model(), clock);
    }
}

sim::spatial_state::Unit& Match::project_spatial(sim::unit_spawn::Slot& slot) {
    auto& s = spatial_units_.at(slot.unit_index);
    s.id = slot.unit_index;
    s.position = slot.unit->position;
    const auto& unit = slot.record;
    const auto& owner = *oa::world_unit_owner(&state(), &unit);
    s.cell = {unit.cell_x, unit.cell_z};
    s.footprint = {unit.footprint_x, unit.footprint_z};
    // object_present means the unit has a movement object; a building has
    // none, even once finished, and its footprint is always a wall.
    s.object_present = movement_[slot.unit_index] != nullptr;
    s.owner_object_present = owner.in_use != 0;
    s.owner_status = owner.status;
    s.spatial_link_locked = unit.attach_parent != 0;
    s.first_attachment = link_first_child(unit);
    s.next_in_bucket = link_next(unit);
    if (const auto* metadata = fields(slot).runtime_metadata) {
        s.max_slope = metadata->max_slope;
        s.max_water_slope = metadata->max_water_slope;
        s.max_water_depth = metadata->max_water_depth;
        s.min_water_depth = metadata->min_water_depth;
    }
    if (const auto entry = type_entry(world_.types, *slot.unit))
        s.bm_code = std::bit_cast<int8_t>(world_.types[*entry].bm_code);
    s.flags = slot.unit->flags;
    s.yard_open = (slot.record.build_flags & 4) != 0;
    s.yard_mask = unit_yard(slot);
    return s;
}

void Match::prepare_spatial_state() {
    ++spatial_query_counters_.projection_preparations;
    spatial_query_counters_.projected_slots += slots_.size();
    const auto& world = state();
    for (std::size_t i = 0; i < slots_.size(); ++i)
        fill_projection(world, world.units[i], spatial_units_[i]);
}

void Match::fill_spatial_unit(void* match, sim::spatial_state::Unit& projected) {
    auto& self = *static_cast<Match*>(match);
    const auto& world = self.state();
    const auto index = static_cast<std::size_t>(&projected - self.spatial_units_.data());
    fill_projection(world, world.units[index], projected);
}

sim::spatial_state::Unit& Match::begin_spatial_change(sim::unit_spawn::Slot& slot) {
    auto& projected = project_spatial(slot);
    sim::spatial_state::begin_fill_round(spatial_, projected);
    spatial_.fill_unit = &Match::fill_spatial_unit;
    spatial_.fill_context = this;
    return projected;
}

void Match::end_spatial_change(bool write_back) {
    spatial_.fill_unit = nullptr;
    spatial_.fill_context = nullptr;
    if (!write_back)
        return;
    // The units the change never reached hold what synchronize_spatial_state
    // would write back to them already.
    auto& world = state();
    for (auto id = spatial_.last_filled;
         id != sim::spatial_state::no_unit && id < spatial_units_.size();
         id = spatial_units_[id].previous_filled) {
        auto& unit = world.units[id];
        unit.flags = spatial_units_[id].flags;
        if (!unit.attach_parent)
            unit.attach_next = oa::oa_unit_ref_from_slot(spatial_units_[id].next_in_bucket);
    }
}

void Match::synchronize_spatial_state() {
    auto& world = state();
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        auto& unit = world.units[i];
        unit.flags = spatial_units_[i].flags;
        if (!unit.attach_parent)
            unit.attach_next = oa::oa_unit_ref_from_slot(spatial_units_[i].next_in_bucket);
    }
}

const formats::objects3d::UnitTypeBounds*
Match::bounds_for(const sim::simulation_state::Unit& unit) const {
    if (const auto entry = type_entry(input_.types, unit); entry && type_bounds_[*entry])
        return &*type_bounds_[*entry];
    return nullptr;
}

void Match::place_unit(
    sim::unit_spawn::Slot& slot, int32_t x, int32_t y, int32_t z, uint8_t layer
) {
    const auto cell_for = [](int32_t world, int16_t footprint) {
        const auto biased =
            static_cast<uint32_t>(world) - static_cast<uint32_t>(footprint) * 0x80000u + 0x80000u;
        return static_cast<int16_t>(std::bit_cast<int32_t>(biased) >> 20);
    };
    const auto cell_x = cell_for(x, slot.record.footprint_x);
    const auto cell_z = cell_for(z, slot.record.footprint_z);
    const std::array position{
        static_cast<uint32_t>(x), static_cast<uint32_t>(y), static_cast<uint32_t>(z)
    };
    if (cell_x == slot.record.cell_x && cell_z == slot.record.cell_z &&
        (slot.unit->flags & 3u) == layer) {
        slot.unit->position = position;
        slot.unit->flags |= OA_UNIT_FLAG_POSITION_DIRTY;
        return;
    }
    auto& projected = begin_spatial_change(slot);
    spatial_.tick = simulation_.tick;
    const auto removed = sim::spatial_state::remove_occupancy(projected, spatial_, map_listeners_);
    end_spatial_change(true);
    if (removed != sim::spatial_state::Error::none) {
        fault_.note("occupancy removal rejected spatial state");
        return;
    }
    slot.unit->position = position;
    slot.unit->flags = (slot.unit->flags & ~3u) | (layer & 3u);
    slot.record.cell_x = cell_x;
    slot.record.cell_z = cell_z;
    if (auto* ground = ground_runtime(slot.unit_index))
        ground->project_slot();
    register_occupancy(slot);
    slot.unit->flags |= OA_UNIT_FLAG_POSITION_DIRTY;
    update_moving_sight(slot);
}

void Match::begin_commander_placement() noexcept {
    commander_placement_ = CommanderPlacement::none;
    auto& world = state();
    const auto local = world.game.local_player_index;
    if (local >= OA_PLAYER_COUNT)
        return;
    uint32_t count = 0;
    const Unit* first = world_player_units(&world, &world.game.players[local], &count);
    if (first == nullptr || (first->flags & OA_UNIT_FLAG_LIVE) == 0)
        return;
    commander_placement_ = CommanderPlacement::placing;
}

bool Match::place_commander(int32_t x, int32_t z) noexcept {
    if (commander_placement_ != CommanderPlacement::placing)
        return false;
    auto& world = state();
    if (x < 0 || z < 0 || x >= world.game.map_pixel_width || z >= world.game.map_pixel_height)
        return false;
    uint32_t count = 0;
    Unit* first =
        world_player_units(&world, &world.game.players[world.game.local_player_index], &count);
    if (first == nullptr || (first->flags & OA_UNIT_FLAG_LIVE) == 0)
        return false;
    auto& slot = slots_[world_unit_slot(&world, first)];
    if (!slot.unit)
        return false;
    // The point's whole part with the commander's own fraction.
    constexpr uint32_t fraction_mask = 0xffffu;
    const auto with_whole = [](uint32_t word, int32_t whole) {
        return std::bit_cast<int32_t>(
            (static_cast<uint32_t>(whole) << 16) | (word & fraction_mask)
        );
    };
    const std::array<uint32_t, 3> position = slot.unit->position;
    place_unit(
        slot,
        with_whole(position[0], x),
        std::bit_cast<int32_t>(position[1]),
        with_whole(position[2], z),
        static_cast<uint8_t>(slot.unit->flags & OA_UNIT_FLAG_OCCUPANCY_MASK)
    );
    return true;
}

void Match::finish_commander_placement() noexcept {
    if (commander_placement_ != CommanderPlacement::placing)
        return;
    commander_placement_ = CommanderPlacement::waiting;
    if (multiplayer.commander_placed != nullptr)
        multiplayer.commander_placed(multiplayer.context);
}

void Match::refresh_restored_footprint(uint16_t index) {
    auto& slot = slots_.at(index);
    auto& projected = begin_spatial_change(slot);
    spatial_.tick = simulation_.tick;
    const auto result =
        sim::spatial_state::refresh_footprint_occupancy(projected, spatial_, map_listeners_);
    end_spatial_change(true);
    if (result != sim::spatial_state::Error::none)
        fault_.note("restored footprint refresh rejected spatial state");
}

bool Match::restore_mobility(
    uint16_t unit, const std::array<uint8_t, sim::ground_orders::mobility_record_size>& saved
) {
    auto* runtime = ground_runtime(unit);
    if (runtime == nullptr)
        return false;
    runtime->load_mobility(saved);
    spatial_units_.at(unit).object_tick = runtime->occupancy_changed_tick;
    return true;
}

void Match::register_occupancy(sim::unit_spawn::Slot& slot) {
    // Collision can change flags on previously inserted units as well as this unit.
    auto& s = begin_spatial_change(slot);
    spatial_.tick = simulation_.tick;
    const auto result = sim::spatial_state::register_unit(s, spatial_, map_listeners_);
    if (slot.unit->object_present && movement_[slot.unit_index])
        movement_[slot.unit_index]->occupancy_changed_tick = s.object_tick;
    if (result != sim::spatial_state::Error::none) {
        end_spatial_change(false);
        fault_.note(
            "unit spatial registration rejected branch/state",
            std::to_string(static_cast<int>(result))
        );
        return;
    }
    end_spatial_change(true);
}

void Match::notify_created(sim::unit_spawn::Slot& slot) {
    // Nothing is shared while the run flag (Game.session_flags bit 0) is
    // clear.
    if (!simulation_.run_flag)
        return;
    if (multiplayer.unit_created) {
        multiplayer.unit_created(multiplayer.context, slot.unit_index);
        return;
    }
    fault_.note("unit creation shared without a multiplayer handler");
}

void Match::notify_finished(sim::unit_spawn::Slot& slot) {
    // A building created finished names itself as its builder.
    notify_finished(slot, slot);
}

void Match::notify_finished(sim::unit_spawn::Slot& unit, sim::unit_spawn::Slot& builder) {
    if (!simulation_.run_flag || multiplayer.unit_finished == nullptr || !unit.unit ||
        !sim::simulation_state::locally_simulated(*unit.unit))
        return;
    multiplayer.unit_finished(multiplayer.context, unit.unit_index, builder.unit_index);
}

void Match::end_local_game() noexcept {
    // A game already decided ends this way too: a victory becomes a defeat.
    outcome_state_.flags |= sim::scenario::outcome_flag::finished;
    outcome_state_.flags =
        static_cast<uint16_t>(outcome_state_.flags & ~sim::scenario::outcome_flag::won);
    outcome_result_ = sim::scenario::Outcome::defeat;
    local_game_ended_ = true;
}

void Match::set_activation(sim::unit_spawn::Slot& slot, uint8_t mask, bool enabled) {
    auto wake = [this, &slot](uint32_t event) { wake_target_observers(*slot.unit, event); };
    Activation host(
        slot, instance(slot.unit_index), services_, wake, simulation_, multiplayer, fault_
    );
    sim::unit_activation::change(slot.record.state_flags, slot.unit_index, mask, enabled, host);
}

std::span<const uint16_t> Match::player_coverage(uint8_t owner) const {
    if (owner >= other_player_coverage_.size()) {
        fault_.note("coverage owner outside player table");
        return {};
    }
    return owner == input_.viewpoint_player
               ? std::span<const uint16_t>(sight_.coverage)
               : std::span<const uint16_t>(other_player_coverage_[owner]);
}

bool Match::unit_visible(uint8_t player, uint16_t index) const {
    return unit_in_sight(player, index, sim::visibility_state::SightReading::simulation);
}

bool Match::unit_shown(uint8_t player, uint16_t index) const {
    return unit_in_sight(player, index, sim::visibility_state::SightReading::player);
}

bool Match::unit_in_sight(
    uint8_t player, uint16_t index, sim::visibility_state::SightReading reading
) const {
    if (player >= simulation_.players.size()) {
        fault_.note("visibility owner outside player table");
        return false;
    }
    const auto& slot = slots_.at(index);
    const auto& unit = *slot.unit;
    // Allied vision shows every unit whose owner allies the player, cloaked
    // or out of sight; the player's own units pass through its own row.
    if (rules().intel.allied_los_sharing.enabled) {
        if (owner_allies(slot.record, player))
            return true;
    } else if (unit.owner == &simulation_.players[player])
        return true;
    if (slot.record.state_flags & 4)
        return false;
    const formats::objects3d::UnitTypeBounds* bounds = bounds_for(unit);
    if (!bounds) {
        fault_.note("visibility query requires resolved unit bounds");
        return false;
    }
    std::array<uint32_t, 3> point = unit.position;
    point[0] += static_cast<uint32_t>(bounds->bounds_min_x);
    point[1] += static_cast<uint32_t>(bounds->model_height);
    point[2] += static_cast<uint32_t>(bounds->bounds_min_z);
    if (!(unit.flags & OA_UNIT_FLAG_VIEWPOINT_OWNED) &&
        std::bit_cast<int32_t>(point[1]) < static_cast<int32_t>(simulation_.sea_level) * 65536)
        return false;
    if (point_in_sight(player, point, reading))
        return true;
    point[0] += static_cast<uint32_t>(bounds->size_x);
    if (point_in_sight(player, point, reading))
        return true;
    point[2] += static_cast<uint32_t>(bounds->size_z);
    point[1] -= static_cast<uint32_t>(bounds->size_y);
    if (point_in_sight(player, point, reading))
        return true;
    point[0] -= static_cast<uint32_t>(bounds->size_x);
    return point_in_sight(player, point, reading);
}

namespace {
// The sight cell under a 16.16 point, raised by half its height, or nothing
// past the grid (a negative cell fails the unsigned test).
std::optional<std::size_t> sight_cell(
    const sim::visibility_state::PlayerSightGrid& sight, const std::array<uint32_t, 3>& position
) {
    const auto x = static_cast<int32_t>(low(position[0] >> 16)) >> 5;
    const auto z = (static_cast<int32_t>(low(position[2] >> 16)) -
                    (static_cast<int32_t>(low(position[1] >> 16)) >> 1)) >>
                   5;
    if (static_cast<uint32_t>(x) >= static_cast<uint32_t>(sight.width) ||
        static_cast<uint32_t>(z) >= static_cast<uint32_t>(sight.height))
        return std::nullopt;
    return static_cast<std::size_t>(z) * static_cast<std::size_t>(sight.width) +
           static_cast<std::size_t>(x);
}
} // namespace

bool Match::point_visible(uint8_t player, const std::array<uint32_t, 3>& position) const {
    return point_in_sight(player, position, sim::visibility_state::SightReading::simulation);
}

bool Match::point_shown(uint8_t player, const std::array<uint32_t, 3>& position) const {
    return point_in_sight(player, position, sim::visibility_state::SightReading::player);
}

bool Match::point_in_sight(
    uint8_t player,
    const std::array<uint32_t, 3>& position,
    sim::visibility_state::SightReading reading
) const {
    const auto coverage = player_coverage(player);
    if ((state().game.visibility_flags & sim::visibility_state::update_sight_grid) == 0)
        return point_mapped(position);
    const auto index = sight_cell(sight_, position);
    return index && *index < coverage.size() &&
           sim::visibility_state::count_in_sight(coverage[*index], reading);
}

bool Match::cell_or_footprint_corner_shown(
    uint8_t player,
    int16_t cell_x,
    int16_t cell_z,
    int16_t footprint_x,
    int16_t footprint_z,
    int16_t height
) const {
    constexpr int32_t units_per_cell = 16;
    // The whole-unit words go in the high halves; the fractions are zero.
    const auto point = [&](int32_t x, int32_t z) {
        return std::array<uint32_t, 3>{
            static_cast<uint32_t>(static_cast<uint16_t>(x)) << 16U,
            static_cast<uint32_t>(static_cast<uint16_t>(height)) << 16U,
            static_cast<uint32_t>(static_cast<uint16_t>(z)) << 16U
        };
    };
    const auto x = cell_x * units_per_cell;
    const auto z = cell_z * units_per_cell;
    return point_shown(player, point(x, z)) ||
           point_shown(
               player, point(x + footprint_x * units_per_cell, z + footprint_z * units_per_cell)
           );
}

bool Match::point_mapped(const std::array<uint32_t, 3>& position) const {
    const auto index = sight_cell(sight_, position);
    if (!index)
        return false;
    // Deliberately the viewpoint bit, not the queried player's bit.
    return (sight_.player_bits[*index] & (1u << sight_.viewpoint_player)) != 0;
}

bool Match::player_mapped(uint8_t player, const std::array<uint32_t, 3>& position) const {
    if (player >= OA_PLAYER_COUNT)
        return false;
    const auto index = sight_cell(sight_, position);
    if (!index)
        return false;
    return (sight_.player_bits[*index] & (1u << player)) != 0;
}

bool Match::point_seen_by(uint8_t player, const std::array<uint32_t, 3>& position) const {
    if (player == state().game.viewpoint_player ||
        (state().game.visibility_flags & sim::visibility_state::update_sight_grid) != 0)
        return point_visible(player, position);
    const auto index = sight_cell(sight_, position);
    return index && player < OA_PLAYER_COUNT && (sight_.player_bits[*index] & (1u << player)) != 0;
}

sim::visibility_state::SightContext Match::sight_context() {
    sim::visibility_state::SightContext context{};
    context.grid = &sight_;
    for (std::size_t player = 0; player < other_player_coverage_.size(); ++player)
        context.coverage[player] = player == input_.viewpoint_player
                                       ? std::span<uint16_t>(sight_.coverage)
                                       : std::span<uint16_t>(other_player_coverage_[player]);
    context.masks = input_.sight_masks;
    context.altitude = input_.altitude_sight ? &*input_.altitude_sight : nullptr;
    context.visibility_flags = state().game.visibility_flags;
    context.minimum_height_cell = simulation_.sea_level;
    context.allied_vision = rules().intel.allied_los_sharing.enabled;
    return context;
}

void Match::update_sight(sim::unit_spawn::Slot& slot) {
    auto& world = state();
    const auto* def = oa::world_unit_def_of(&world, &slot.record);
    const auto* owner = oa::world_unit_owner(&world, &slot.record);
    if (def == nullptr || owner == nullptr) {
        fault_.note("sight stamp requires the unit's type and owner");
        return;
    }
    auto context = sight_context();
    sim::visibility_state::stamp_unit_sight(slot.record, *def, *owner, context);
}

void Match::update_moving_sight(sim::unit_spawn::Slot& slot) {
    auto& world = state();
    const auto* def = oa::world_unit_def_of(&world, &slot.record);
    const auto* owner = oa::world_unit_owner(&world, &slot.record);
    if (def == nullptr || owner == nullptr) {
        fault_.note("sight stamp requires the unit's type and owner");
        return;
    }
    auto context = sight_context();
    sim::visibility_state::move_unit_sight(slot.record, *def, *owner, context);
}

void Match::update_player_sight(const oa::Player& player) {
    auto& world = state();
    uint32_t count = 0;
    const oa::Unit* first = oa::world_player_units(&world, &player, &count);
    for (uint32_t i = 0; i < count; ++i) {
        const auto index = oa::world_unit_slot(&world, &first[i]);
        if ((first[i].flags & OA_UNIT_FLAG_LIVE) != 0 && index < slots_.size())
            update_moving_sight(slots_[index]);
    }
}

void Match::reset_sight_buffers(bool refill_mapped) {
    auto& game = state().game;
    if (refill_mapped) {
        const bool mapping = (game.visibility_flags & sim::visibility_state::terrain_mapping) != 0;
        std::fill(
            sight_.player_bits.begin(),
            sight_.player_bits.end(),
            mapping ? mapped_by_nobody : mapped_by_all
        );
    }
    const bool line_of_sight =
        (game.visibility_flags & sim::visibility_state::update_sight_grid) != 0;
    for (uint8_t index = 0; index < OA_PLAYER_COUNT; ++index) {
        if (!sim::simulation_state::player_slot_active(index, game.players[index]))
            continue;
        auto& coverage =
            index == input_.viewpoint_player ? sight_.coverage : other_player_coverage_[index];
        std::fill(coverage.begin(), coverage.end(), line_of_sight ? unseen : seen_once);
    }
    for (uint32_t slot = 1; slot < state().unit_slot_count; ++slot)
        if (state().units[slot].type_index != 0)
            update_sight(slots_[slot]);
    game.visibility_flags =
        static_cast<uint8_t>(game.visibility_flags & ~OA_VISIBILITY_FOG_MASK_CURRENT);
    game.radar_blink_flags = static_cast<uint16_t>(game.radar_blink_flags | OA_RADAR_MAPPED_DIRTY);
}

void Match::notify_scenario_created(sim::unit_spawn::Slot& slot) {
    sim::scenario::notify_unit_created(scenario_, {reinterpret_cast<uintptr_t>(slot.unit)});
}

std::array<uint32_t, 3> Match::piece_world_position(sim::unit_spawn::Slot& slot, uint32_t piece) {
    auto* unit = instance(slot.unit_index);
    if (!unit) {
        fault_.note("piece query before instance");
        return slot.unit ? slot.unit->position : std::array<uint32_t, 3>{};
    }
    return unit->piece_world(piece);
}

uint16_t Match::direction_to(uint32_t x, uint32_t z) {
    return base::game_math::direction(std::bit_cast<int32_t>(x), std::bit_cast<int32_t>(z));
}

uint32_t Match::distance(uint32_t x, uint32_t z) {
    return base::game_math::distance(std::bit_cast<int32_t>(x), std::bit_cast<int32_t>(z));
}

int32_t Match::sample_terrain_height(uint32_t x, uint32_t z) {
    return terrain_.height(std::bit_cast<int32_t>(x), std::bit_cast<int32_t>(z));
}

std::vector<Match::NanoLaser> Match::nano_lasers() const {
    std::vector<NanoLaser> beams;
    auto add = [&](const std::array<uint32_t, 3>& from, const sim::simulation_state::Unit& target) {
        const formats::objects3d::UnitTypeBounds* bounds = bounds_for(target);
        const std::array<uint32_t, 3> position = target.position;
        std::array<int32_t, 3> min_p{
            std::bit_cast<int32_t>(position[0]),
            std::bit_cast<int32_t>(position[1]),
            std::bit_cast<int32_t>(position[2])
        };
        std::array<int32_t, 3> max_p = min_p;
        if (bounds) {
            min_p[0] += bounds->bounds_min_x;
            min_p[1] += bounds->bounds_min_y;
            min_p[2] += bounds->bounds_min_z;
            max_p[0] += bounds->bounds_max_x;
            max_p[1] += bounds->model_height;
            max_p[2] += bounds->bounds_max_z;
        }
        NanoLaser beam;
        beam.from = from;
        for (int i = 0; i < 3; ++i) {
            const auto delta =
                max_p[static_cast<std::size_t>(i)] - min_p[static_cast<std::size_t>(i)];
            // Origin = min + 4/11 delta, extent = 3/11 delta.
            beam.to_origin[static_cast<std::size_t>(i)] =
                (delta * 4) / 11 + min_p[static_cast<std::size_t>(i)];
            beam.to_extent[static_cast<std::size_t>(i)] = (delta * 3) / 11;
        }
        beams.push_back(beam);
    };
    auto nano_from = [&](const sim::unit_spawn::Slot& builder) -> std::array<uint32_t, 3> {
        auto* inst = const_cast<Match*>(this)->instance(builder.unit_index);
        if (inst)
            return inst->query_nano_world();
        return builder.unit->position;
    };
    // Each slot views a unit of its own: the slot at the unit's id, unless
    // that slot views another unit, when the pool is walked for it.
    auto slot_viewing =
        [&](const sim::simulation_state::Unit* unit) -> const sim::unit_spawn::Slot* {
        const auto id = unit->record.id;
        if (id < slots_.size() && slots_[id].unit == unit)
            return &slots_[id];
        for (const auto& slot : slots_)
            if (slot.unit == unit)
                return &slot;
        return nullptr;
    };
    for (const auto& entry : orders_) {
        if (!entry || !entry->unit || !entry->construction.target)
            continue;
        if (entry->order.kind == get_built_kind) {
            const auto* worker = slot_viewing(entry->construction.target);
            add(worker ? nano_from(*worker) : entry->construction.target->position, *entry->unit);
            continue;
        }
        const auto* builder = slot_viewing(entry->unit);
        add(builder ? nano_from(*builder) : entry->unit->position, *entry->construction.target);
    }
    // The units in their build stance, in pool order, listed at the first
    // unfinished unit. With more than the list holds, every unfinished unit
    // walks the whole pool for them instead.
    constexpr std::size_t listed_stance_capacity = 512;
    std::array<const sim::unit_spawn::Slot*, listed_stance_capacity> stances{};
    std::size_t stance_count = 0;
    bool stances_listed = false;
    bool stances_fit = true;
    for (const auto& slot : world_.slots) {
        if (slot.unit == nullptr || slot.record.build_remaining == 0.0F)
            continue;
        if (!stances_listed) {
            for (const auto& builder : world_.slots) {
                if (builder.unit == nullptr || (builder.record.build_flags & 1) == 0)
                    continue;
                if (stance_count == stances.size()) {
                    stances_fit = false;
                    break;
                }
                stances[stance_count++] = &builder;
            }
            stances_listed = true;
        }
        const auto beam_from = [&](const sim::unit_spawn::Slot& builder) {
            if (builder.unit == nullptr || builder.unit_index == slot.unit_index)
                return;
            if (builder.record.owner_index != slot.record.owner_index ||
                (builder.record.build_flags & 1) == 0)
                return;
            add(nano_from(builder), *slot.unit);
        };
        if (stances_fit)
            for (std::size_t i = 0; i < stance_count; ++i)
                beam_from(*stances[i]);
        else
            for (const auto& builder : world_.slots)
                beam_from(builder);
    }
    return beams;
}

void Match::set_yard_open(sim::unit_spawn::Slot& slot, int32_t value) {
    prepare_spatial_state();
    auto& unit = project_spatial(slot);
    (void)sim::spatial_state::change_yard(unit, value, spatial_, map_listeners_);
    for (std::size_t i = 0; i < slots_.size(); ++i)
        state().units[i].flags = spatial_units_[i].flags;
    slot.record.build_flags =
        static_cast<uint8_t>((slot.record.build_flags & ~4u) | (unit.yard_open ? 4u : 0u));
}

uint32_t Match::loaded_child_count(uint16_t index) const {
    const auto& unit = match_unit(*this, index);
    return static_cast<uint32_t>(sim::unit_spawn::attached_child_count(state(), unit));
}

void Match::keep_rules(const OfflineInputs& input) {
    unit_type_rule_count_ = input.unit_type_rules.size();
    if (unit_type_rule_count_ != 0) {
        unit_type_rules_ =
            std::make_unique<data::match_rules::UnitTypeRules[]>(unit_type_rule_count_);
        std::copy(
            input.unit_type_rules.begin(), input.unit_type_rules.end(), unit_type_rules_.get()
        );
    }
    weapon_rule_count_ = input.weapon_rules.size();
    if (weapon_rule_count_ != 0) {
        weapon_rules_ = std::make_unique<data::match_rules::WeaponTypeRules[]>(weapon_rule_count_);
        std::copy(input.weapon_rules.begin(), input.weapon_rules.end(), weapon_rules_.get());
    }
    // The inputs the match keeps point at its own copies, never at the caller's.
    input_.unit_type_rules = {unit_type_rules_.get(), unit_type_rule_count_};
    input_.weapon_rules = {weapon_rules_.get(), weapon_rule_count_};
    value_rules = rules_view();
    value_limits = &input_.limits;
    value_features = enhanced_features_;
    keep_build_site_kickout();
    keep_structure_gifts();
}

data::match_rules::MatchRulesView Match::rules_view() const noexcept {
    return {
        &input_.rules,
        {unit_type_rules_.get(), unit_type_rule_count_},
        {weapon_rules_.get(), weapon_rule_count_}
    };
}

uint64_t Match::fold_rule_state(uint64_t digest) const noexcept {
    std::span<const uint8_t> held{};
    if (input_.mutator_digest)
        held = *input_.mutator_digest;
    const auto digested_count = digested_rule_state_count(rule_state_);
    if (digested_count == 0 && held.empty())
        return digest;
    std::span<const uint8_t> profile_hash{};
    if (digested_count != 0 && input_.profile_sim_hash)
        profile_hash = *input_.profile_sim_hash;
    return digest_rule_state(digest, rule_state_, profile_hash, held);
}

} // namespace oa::sim::match_runtime
