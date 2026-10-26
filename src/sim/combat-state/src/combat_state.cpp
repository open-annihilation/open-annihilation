// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/combat_state.hpp"
#include "oa/base/game_math.hpp"
#include "oa/core/weapon_def.h"
#include <algorithm>
#include <cmath>
#include <bit>
#include <limits>
#include <memory>
#include <utility>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace oa::sim::combat_state {
using base::game_math::truncate_low32;

namespace {
constexpr uint8_t preserved_flag_bits = 0xf0;
constexpr uint8_t initialized_flag = 0x10;
constexpr uint8_t nondefault_definition_flag = 0x02;
constexpr unsigned slot_index_shift = 2;

/// Arms one slot: its definition, a cleared stockpile, its flags and its muzzle offset.
void initialize_slot(
    WeaponSlot& slot, const WeaponDefinition* definition, size_t index, const SlotGeometry& geometry
) noexcept {
    slot.definition = definition;
    slot.stockpile = 0;
    slot.flags = static_cast<uint8_t>(
        (slot.flags & preserved_flag_bits) | initialized_flag |
        (static_cast<uint8_t>(index) << slot_index_shift) |
        (definition->registry_index != 0 ? nondefault_definition_flag : 0)
    );
    const auto difference = static_cast<int32_t>(
        static_cast<uint32_t>(geometry.muzzle_z) - static_cast<uint32_t>(geometry.aim_from_z)
    );
    slot.muzzle_offset = truncate_low32(static_cast<double>(difference) * weapon_range_scale);
}

/// Returns 1.25 times the wrapped Z difference of the two pieces, truncated toward
/// zero with the low 32 bits kept.
int32_t calculate_muzzle_offset(const SlotGeometry& geometry) noexcept {
    const auto difference = static_cast<int32_t>(
        static_cast<uint32_t>(geometry.muzzle_z) - static_cast<uint32_t>(geometry.aim_from_z)
    );
    return truncate_low32(static_cast<double>(difference) * weapon_range_scale);
}
} // namespace

InitializationResult initialize_weapon_slots(
    UnitWeapons& unit, const std::array<SlotGeometry, weapon_slot_count>& geometry
) noexcept {
    uint16_t maximum_reload_ticks = 0;
    for (size_t index = 0; index < weapon_slot_count; ++index) {
        auto& slot = unit.slots[index];
        const auto* definition = unit.definitions[index];
        initialize_slot(slot, definition, index, geometry[index]);
        maximum_reload_ticks = std::max(maximum_reload_ticks, definition->reload_time_ticks);
    }
    return {
        static_cast<int32_t>(maximum_reload_ticks) * milliseconds_per_second /
        simulation_ticks_per_second
    };
}

void WeaponRegistry::allocate(std::size_t capacity) {
    definitions_ = std::make_unique<WeaponDefinition[]>(capacity);
    names_ = std::make_unique<SlotName[]>(capacity);
    records_ = std::make_unique<WeaponDef[]>(capacity);
    capacity_ = capacity;
    for (std::size_t slot = 0; slot < capacity_; ++slot) {
        const auto index = static_cast<uint16_t>(slot);
        definitions_[slot].registry_index = index;
        oa_weapon_def_set_id(&records_[slot], index);
    }
}

WeaponRegistry::WeaponRegistry(std::size_t capacity) {
    allocate(capacity);
}

WeaponRegistry::WeaponRegistry(const WeaponRegistry& other) {
    allocate(other.capacity_);
    for (std::size_t slot = 0; slot < capacity_; ++slot) {
        definitions_[slot] = other.definitions_[slot];
        names_[slot] = other.names_[slot];
        records_[slot] = other.records_[slot];
    }
}

WeaponRegistry& WeaponRegistry::operator=(const WeaponRegistry& other) {
    if (this == &other)
        return *this;
    if (capacity_ != other.capacity_)
        allocate(other.capacity_);
    for (std::size_t slot = 0; slot < capacity_; ++slot) {
        definitions_[slot] = other.definitions_[slot];
        names_[slot] = other.names_[slot];
        records_[slot] = other.records_[slot];
    }
    return *this;
}

WeaponRegistry::WeaponRegistry(WeaponRegistry&& other) noexcept
    : definitions_(std::move(other.definitions_)), names_(std::move(other.names_)),
      records_(std::move(other.records_)), capacity_(other.capacity_) {
    other.capacity_ = 0;
}

WeaponRegistry& WeaponRegistry::operator=(WeaponRegistry&& other) noexcept {
    if (this != &other) {
        definitions_ = std::move(other.definitions_);
        names_ = std::move(other.names_);
        records_ = std::move(other.records_);
        capacity_ = other.capacity_;
        other.capacity_ = 0;
    }
    return *this;
}

void WeaponRegistry::grow(std::size_t capacity) {
    if (capacity <= capacity_)
        return;
    auto definitions = std::make_unique<WeaponDefinition[]>(capacity);
    auto names = std::make_unique<SlotName[]>(capacity);
    auto records = std::make_unique<WeaponDef[]>(capacity);
    for (std::size_t slot = 0; slot < capacity_; ++slot) {
        definitions[slot] = std::move(definitions_[slot]);
        names[slot] = std::move(names_[slot]);
        records[slot] = records_[slot];
    }
    for (std::size_t slot = capacity_; slot < capacity; ++slot) {
        const auto index = static_cast<uint16_t>(slot);
        definitions[slot].registry_index = index;
        oa_weapon_def_set_id(&records[slot], index);
    }
    definitions_ = std::move(definitions);
    names_ = std::move(names);
    records_ = std::move(records);
    capacity_ = capacity;
}

void WeaponRegistry::install(const WeaponDef& weapon, const data::defs::WeaponAssetNames& assets) {
    const uint16_t index = oa_weapon_def_id(&weapon);
    if (static_cast<std::size_t>(index) >= capacity_)
        grow(static_cast<std::size_t>(index) + 1u);
    records_[index] = weapon;
    auto& definition = definitions_[index];
    definition.reload_time_ticks = static_cast<uint16_t>(weapon.reload_time);
    definition.default_damage = static_cast<uint16_t>(weapon.damage_default);
    definition.projectile_velocity = weapon.weapon_velocity;
    definition.minimum_barrel_angle_radians = weapon.min_barrel_angle;
    definition.range_world_units = weapon.range;
    definition.flags = weapon.flags;
    definition.energy_per_shot = weapon.energy_per_shot;
    definition.metal_per_shot = weapon.metal_per_shot;
    definition.weapontimer_ticks = static_cast<uint16_t>(weapon.weapon_timer);
    definition.areaofeffect = static_cast<uint16_t>(weapon.area_of_effect);
    definition.accuracy = weapon.accuracy;
    definition.tolerance = static_cast<uint16_t>(weapon.tolerance);
    definition.pitch_tolerance = static_cast<uint16_t>(weapon.pitch_tolerance);
    definition.start_velocity = weapon.start_velocity;
    definition.acceleration = weapon.weapon_acceleration;
    definition.turn_rate = static_cast<uint16_t>(weapon.turn_rate);
    definition.burst = static_cast<uint16_t>(weapon.burst);
    definition.burst_rate_ticks = static_cast<uint16_t>(weapon.burst_rate);
    definition.rendertype = static_cast<uint8_t>(weapon.render_type);
    definition.color = static_cast<uint8_t>(weapon.color);
    definition.color2 = static_cast<uint8_t>(weapon.color2);
    definition.explosion_gaf = assets.explosion_gaf;
    definition.explosion_art = assets.explosion_art;
    definition.water_explosion_gaf = assets.water_explosion_gaf;
    definition.water_explosion_art = assets.water_explosion_art;
    definition.soundstart = assets.sound_start;
    definition.soundhit = assets.sound_hit;
    definition.soundwater = assets.sound_water;
    definition.shake_magnitude = weapon.shake_magnitude;
    definition.shake_duration_ticks = weapon.shake_duration;
    definition.edge_effectiveness = weapon.edge_effectiveness;
    definition.spray_angle = static_cast<uint16_t>(weapon.spray_angle);
    definition.duration_ticks = static_cast<uint16_t>(weapon.duration);
    definition.random_decay_ticks = static_cast<uint16_t>(weapon.random_decay);
    definition.flight_time_ticks = static_cast<uint16_t>(weapon.flight_time);
    definition.smoke_delay_ticks = static_cast<uint16_t>(weapon.smoke_delay);
    definition.coverage = weapon.coverage;
    definition.registry_index = index;
    names_[index] = weapon.key;
}

void WeaponRegistry::install_damage_override(
    uint16_t index, std::string_view unit_name, int32_t amount
) {
    auto& overrides = definitions_[index].damage_overrides;
    std::string name(unit_name);
    for (auto& c : name)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c + ('a' - 'A'));
    for (auto& entry : overrides)
        if (entry.unit_name == name) {
            entry.amount = amount;
            return;
        }
    overrides.push_back({std::move(name), amount});
}

int32_t damage_against(const WeaponDefinition& definition, std::string_view unit_name) noexcept {
    for (const auto& entry : definition.damage_overrides) {
        if (entry.unit_name.size() != unit_name.size())
            continue;
        bool same = true;
        for (size_t i = 0; i < unit_name.size() && same; ++i) {
            auto c = unit_name[i];
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c + ('a' - 'A'));
            same = c == entry.unit_name[i];
        }
        if (same)
            return entry.amount;
    }
    return definition.default_damage;
}

namespace {
char ascii_lower(char value) noexcept {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

bool equal_name(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size())
        return false;
    for (size_t i = 0; i < left.size(); ++i)
        if (ascii_lower(left[i]) != ascii_lower(right[i]))
            return false;
    return true;
}
} // namespace

const WeaponDefinition* WeaponRegistry::find(std::string_view name) const noexcept {
    if (name.empty())
        return nullptr;
    for (std::size_t slot = 0; slot < capacity_; ++slot)
        if (!names_[slot].empty() && equal_name(names_[slot], name))
            return &definitions_[slot];
    return nullptr;
}

size_t install_weapon_table(WeaponRegistry& registry, const data::defs::WeaponTable& table) {
    if (table.defs.size() > registry.capacity())
        registry.grow(table.defs.size());
    size_t installed = 0;
    for (size_t slot = 0; slot < table.defs.size(); ++slot) {
        const WeaponDef& weapon = table.defs[slot];
        if (weapon.key[0] == '\0')
            continue;
        registry.install(weapon, table.assets[slot]);
        // The table keeps a name's entries newest first and a hit takes the
        // first that matches, so the oldest is installed first and the newest
        // replaces it.
        const data::defs::WeaponDamageTable& damage = table.damage[slot];
        for (uint32_t entry = damage.count; entry-- > 0;)
            registry.install_damage_override(
                oa_weapon_def_id(&weapon), damage.entries[entry].unit, damage.entries[entry].damage
            );
        ++installed;
    }
    return installed;
}

size_t install_weapon_files(
    WeaponRegistry& registry, const data::defs::Files& files, bool lava_world, uint32_t weapon_types
) {
    const auto table = std::make_unique<data::defs::WeaponTable>();
    const data::defs::WeaponLoadOptions options{
        nullptr, nullptr, lava_world, false, nullptr, weapon_types
    };
    data::defs::load_weapon_defs(&files, table.get(), &options);
    const size_t installed = install_weapon_table(registry, *table);
    data::defs::weapon_table_free(table.get());
    return installed;
}

size_t install_weapon_text(
    WeaponRegistry& registry, std::string_view text, bool lava_world, uint32_t weapon_types
) {
    const auto table = std::make_unique<data::defs::WeaponTable>();
    data::defs::weapon_table_init(table.get());
    const data::defs::WeaponLoadOptions options{
        nullptr, nullptr, lava_world, false, nullptr, weapon_types
    };
    (void)data::defs::load_weapon_text(
        table.get(), text.data(), static_cast<uint32_t>(text.size()), &options
    );
    const size_t installed = install_weapon_table(registry, *table);
    data::defs::weapon_table_free(table.get());
    return installed;
}

WeaponBinding bind_unit_weapons(
    const WeaponRegistry& registry, const std::array<std::string_view, weapon_slot_count>& names
) noexcept {
    WeaponBinding result;
    for (size_t i = 0; i < weapon_slot_count; ++i) {
        const auto* found = registry.find(names[i]);
        result.weapons.definitions[i] = found ? found : &registry.default_definition();
        result.resolved_nondefault_weapon |= found && found->registry_index != 0;
    }
    return result;
}

SpawnCombatResult initialize_spawn_combat(
    UnitWeapons& weapons,
    const WeaponRegistry& registry,
    const std::array<std::string_view, weapon_slot_count>& names,
    SpawnGeometryHost& host
) {
    SpawnCombatResult result;
    const auto binding = bind_unit_weapons(registry, names);
    weapons.definitions = binding.weapons.definitions;
    result.resolved_nondefault_weapon = binding.resolved_nondefault_weapon;
    uint16_t maximum_reload_ticks = 0;
    for (size_t i = 0; i < weapon_slot_count; ++i) {
        const auto* definition = weapons.definitions[i];
        // The slot is armed before either geometry callback runs.
        weapons.slots[i].definition = definition;
        weapons.slots[i].stockpile = 0;
        weapons.slots[i].flags = static_cast<uint8_t>(
            (weapons.slots[i].flags & preserved_flag_bits) | initialized_flag |
            (static_cast<uint8_t>(i) << slot_index_shift) |
            (definition->registry_index != 0 ? nondefault_definition_flag : 0)
        );
        const auto slot = static_cast<uint8_t>(i);
        const auto muzzle = host.query_weapon_world(slot);
        const auto aim = host.aim_from_world(slot);
        weapons.slots[i].muzzle_offset = calculate_muzzle_offset({muzzle[2], aim[2]});
        const auto* current_definition = weapons.slots[i].definition;
        if (!current_definition) {
            result.definition_cleared = true;
            return result;
        }
        maximum_reload_ticks =
            std::max(maximum_reload_ticks, current_definition->reload_time_ticks);
    }
    result.initialization.maximum_reload_milliseconds = static_cast<int32_t>(maximum_reload_ticks) *
                                                        milliseconds_per_second /
                                                        simulation_ticks_per_second;
    host.set_max_reload_time(result.initialization.maximum_reload_milliseconds);
    return result;
}

namespace {
bool category_bit(std::span<const uint32_t> words, uint16_t category) noexcept {
    const auto word = static_cast<size_t>(category >> 5U);
    return word < words.size() && (words[word] & (uint32_t{1} << (category & 31U))) != 0;
}

uint32_t randomized_distance_score(
    const TargetSource& source, const TargetUnit& candidate, TargetSearchHost& host
) {
    const auto dx = std::bit_cast<int32_t>(source.position[0] - candidate.position[0]);
    const auto dz = std::bit_cast<int32_t>(source.position[2] - candidate.position[2]);
    const auto high = [](int32_t value) {
        const auto square = static_cast<uint64_t>(static_cast<int64_t>(value) * value);
        return static_cast<uint32_t>(square >> 32U);
    };
    return host.random_bounded(high(dx) + high(dz));
}
} // namespace

UnitIdentity select_automatic_target(
    const TargetSource& source, const TargetSearchRequest& request, TargetSearchHost& host
) {
    if (request.weapon_slot >= weapon_slot_count)
        return 0;
    const bool owner_override = source.owner_present && source.owner_status == 2;
    const auto radius = host.search_radius(source, request);
    // The candidates not drawn yet are the front `remaining` entries; a drawn
    // one is swapped with the last of them and drops out of the front.
    const auto candidates = host.gather_nearby(source.owner_spatial_index, source.position, radius);
    size_t remaining = candidates.size();
    UnitIdentity category_match = 0, ordinary = 0;
    int32_t category_match_score = std::numeric_limits<int32_t>::max();
    int32_t ordinary_score = std::numeric_limits<int32_t>::max();
    for (uint32_t sampled = 0; sampled < target_search_sample_limit && remaining != 0; ++sampled) {
        const auto selected = host.random_bounded(static_cast<uint32_t>(remaining));
        const auto index = selected < remaining ? static_cast<size_t>(selected) : size_t{};
        --remaining;
        std::swap(candidates[index], candidates[remaining]);
        const auto& candidate = candidates[remaining];
        if (!unit_is_live_target(candidate.unit_flags) ||
            ((candidate.type_auto_target_flags & candidate_type_auto_target_flag) == 0 &&
             !owner_override && !host.global_target_override()) ||
            ((source.type_flags & source_range_check_bypass_flag) == 0 &&
             !host.weapon_can_reach(source, candidate, request.weapon_slot)) ||
            (!request.explicit_radius &&
             category_bit(source.implicit_excluded_category_mask, candidate.category)) ||
            ((source.weapon_flags[request.weapon_slot] & weapon_paralyzer_low_byte_flag) != 0 &&
             (candidate.candidate_flags & candidate_paralyzed_flag) != 0))
            continue;
        const auto score =
            std::bit_cast<int32_t>(randomized_distance_score(source, candidate, host));
        if (category_bit(
                source.preferred_category_masks[request.weapon_slot], candidate.category
            )) {
            if (score < category_match_score) {
                category_match_score = score;
                category_match = candidate.identity;
            }
        } else if (score < ordinary_score) {
            ordinary_score = score;
            ordinary = candidate.identity;
        }
    }
    // The nearest candidate outside the per-weapon mask wins over any inside it.
    return ordinary != 0 ? ordinary : category_match;
}

void gather_sightings(
    std::span<const uint16_t> seen,
    std::span<const uint16_t> radar,
    bool radar_fallback,
    const std::array<uint32_t, 3>& center,
    int32_t radius,
    IntelligenceHost& host,
    std::vector<TargetUnit>& found
) {
    const auto radius_squared =
        std::bit_cast<int32_t>(static_cast<uint32_t>(radius) * static_cast<uint32_t>(radius));
    const auto append = [&](std::span<const uint16_t> slots) {
        for (const auto slot : slots) {
            const UnitIdentity identity = slot;
            if (!host.unit_active(identity))
                continue;
            const auto* unit = host.resolve(identity);
            if (!unit)
                continue;
            const auto dx = std::bit_cast<int32_t>(center[0] - unit->position[0]);
            const auto dz = std::bit_cast<int32_t>(center[2] - unit->position[2]);
            const auto high = [](int32_t value) {
                return static_cast<uint32_t>(
                    static_cast<uint64_t>(static_cast<int64_t>(value) * value) >> 32U
                );
            };
            if (std::bit_cast<int32_t>(high(dx) + high(dz)) <= radius_squared)
                found.push_back(*unit);
        }
    };
    append(seen);
    if (found.empty() && radar_fallback)
        append(radar);
}

namespace {
uint8_t low_byte(int32_t value) noexcept {
    return static_cast<uint8_t>(static_cast<uint32_t>(value));
}

constexpr float coefficient(uint32_t bits) noexcept {
    return std::bit_cast<float>(bits);
}
} // namespace

int32_t classify_type(const StrategicType& type, const StrategicRefreshContext& context) noexcept {
    const auto resource = energy_rate(
        type.energy_use,
        type.wind_generator,
        type.tidal_generator,
        context.wind_factor,
        context.tidal_strength
    );
    WeaponScoreInput weapon_inputs[3]{};
    for (int slot = 0; slot < 3; ++slot)
        weapon_inputs[slot] = {
            type.weapons[static_cast<size_t>(slot)].registry_index != 0,
            type.weapons[static_cast<size_t>(slot)].range_world_units,
            type.weapons[static_cast<size_t>(slot)].damage_default
        };
    auto classification_base = (type.extracts_metal != 0.0F ? 11 : 1) +
                               (type.makes_metal != 0 ? 10 : 0) + (resource < 0.0F ? 10 : 0);
    classification_base = truncate_low32(
        static_cast<double>(type.build_cost_metal) * coefficient(0x3c23d70aU) + classification_base
    );
    const auto weapon_score =
        threat_score((type.abilities & ability_can_attack) != 0, weapon_inputs);
    return std::clamp(
        truncate_low32(
            static_cast<double>(classification_base) +
            static_cast<double>(type.build_cost_energy) * coefficient(0x3b03126fU)
        ) + static_cast<int8_t>(low_byte(weapon_score)),
        -100,
        100
    );
}

void strategic_refresh(
    StrategicRefreshState& state,
    std::span<const StrategicType> types,
    const StrategicRefreshContext& context
) {
    state.classifications.assign(types.size(), 0);
    state.strengths.assign(types.size(), {});
    state.owned_counts.resize(types.size());
    for (size_t index = 1; index < types.size(); ++index) {
        const auto& type = types[index];
        const auto resource = static_cast<double>(energy_rate(
            type.energy_use,
            type.wind_generator,
            type.tidal_generator,
            context.wind_factor,
            context.tidal_strength
        ));
        const auto classification = classify_type(type, context);
        state.classifications[index] = low_byte(classification);
        auto priority = 1;
        if ((type.abilities & ability_can_attack) != 0)
            priority = 21;
        if ((type.flags & def_flag_builder) != 0 && state.owned_counts[index] < 3)
            priority += 30;
        if (resource < 0.0F)
            priority += 50;
        if (type.extracts_metal != 0.0F)
            priority += 50;
        if (type.makes_metal != 0)
            priority += 25;
        if ((type.flags & def_flag_can_fly) != 0)
            priority += 40;
        if (type.sonar_distance != 0)
            priority += 15;
        if (type.radar_distance != 0)
            priority += 5;
        priority = truncate_low32(
            static_cast<double>(priority) +
            std::clamp(static_cast<double>(type.energy_make), 0.0, 30.0)
        );
        if (state.owned_counts[index] == 0)
            priority = std::bit_cast<int32_t>(static_cast<uint32_t>(priority) << 2U);
        if (state.owned_counts[index] == 1)
            priority = std::bit_cast<int32_t>(static_cast<uint32_t>(priority) << 1U);
        if (type.min_water_depth >= 0)
            priority = std::bit_cast<int32_t>(static_cast<uint32_t>(priority) * 3U);
        if ((context.pool_units_per_player >> 1U) < context.owner_unit_count)
            priority += classification / 2;
        if ((type.abilities & ability_can_load) != 0 || (type.flags & def_flag_is_feature) != 0 ||
            (type.wind_generator != 0.0F && context.wind_max < context.wind_strength_divisor / 2))
            priority = 0;
        if (priority > 99)
            priority = 100;
        state.strengths[index][0] = low_byte(priority);
        const auto rounded_cost = static_cast<float>(
            static_cast<double>(type.build_cost_energy) * coefficient(0xbb23d70aU)
        );
        const auto energy_value =
            std::clamp(static_cast<double>(rounded_cost) - resource * 5.0, 0.0, 100.0);
        state.strengths[index][2] = low_byte(truncate_low32(energy_value));
        const auto metal_value = std::clamp(
            static_cast<double>(type.build_cost_metal) * coefficient(0xbca3d70aU) +
                (type.extracts_metal != 0.0F ? 100.0 : 0.0) + (type.makes_metal != 0 ? 25.0 : 0.0),
            0.0,
            100.0
        );
        state.strengths[index][1] = low_byte(truncate_low32(metal_value));
    }
}

bool issue_attack_order(
    const AttackSource& source, const TargetUnit& target, bool forced, AttackHost& host
) {
    if (source.identity == target.identity ||
        (((source.flags & standing_move_order_mask) == 0) && !forced) ||
        (((source.flags & standing_fire_order_mask) == 0) && !forced))
        return false;
    const auto attack_kind = host.resolve_order(attack_order_kind, source, &target, nullptr);
    if (attack_kind == 0)
        return false;
    std::array<AttackOrderRequest, 2> orders{};
    size_t count = 1;
    if ((source.flags & standing_move_order_mask) == standing_move_order_manoeuvre && !forced) {
        const auto move_kind =
            host.resolve_order(move_order_kind, source, nullptr, &source.position);
        orders[0] = {move_kind, 0, source.position, true, 0, 0, 0};
        orders[1] = {
            attack_kind,
            target.identity,
            {},
            false,
            source.maneuver_leash_length,
            std::bit_cast<int16_t>(static_cast<uint16_t>(source.position[0] >> 16U)),
            std::bit_cast<int16_t>(static_cast<uint16_t>(source.position[2] >> 16U))
        };
        count = 2;
    } else
        orders[0] = {attack_kind, target.identity, {}, false, 0, 0, 0};
    return host.commit_orders(source, std::span<const AttackOrderRequest>(orders.data(), count));
}

int threat_score(bool can_attack, const WeaponScoreInput weapons[3]) noexcept {
    int score = can_attack ? 0xb : 1;
    for (int slot = 0; slot < 3; ++slot) {
        if (!weapons[slot].present)
            continue;
        score += weapons[slot].range_world_units / 100 + 5 +
                 static_cast<int>(weapons[slot].default_damage) / 0x28;
    }
    if (score > 100)
        return 100;
    if (score < -100)
        return -100;
    return score;
}

float energy_rate(
    float energy_use,
    float wind_generator,
    float tidal_generator,
    float wind_factor,
    float tidal_factor
) noexcept {
    if (energy_use != 0.0F)
        return energy_use;
    if (!(wind_generator <= 0.0F))
        return -wind_factor * wind_generator;
    if (!(tidal_generator <= 0.0F))
        return -tidal_factor * tidal_generator;
    return 0.0F;
}
} // namespace oa::sim::combat_state
