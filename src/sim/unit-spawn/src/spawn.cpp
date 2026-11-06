// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/unit_spawn/spawn.hpp"
#include <bit>
#include <cstdint>

namespace oa::sim::unit_spawn {
namespace {
constexpr uint8_t periodic_enabled = 2;
constexpr uint8_t no_player = 10;
constexpr uint16_t player_slot_mask = (1u << OA_PLAYER_COUNT) - 1u;
constexpr std::size_t wire_unit_slot_capacity = 1u << 15;

// Unit.flags a spawn clears, then sets: the unit occupies the ground layer,
// at move rate 0, unselected, unjammed, not cloaking and not carried; it is
// selectable and its position is marked changed.
constexpr uint32_t spawn_cleared_flags =
    OA_UNIT_FLAG_OCCUPANCY_MASK | OA_UNIT_FLAG_MOVE_RATE_MASK | OA_UNIT_FLAG_SELECTED |
    OA_UNIT_FLAG_JAMMED | OA_UNIT_FLAG_CLOAK_RUNNING | OA_UNIT_FLAG_ATTACHED_WITHOUT_PIECE;
constexpr uint32_t spawn_set_flags =
    ground_occupancy_state | OA_UNIT_FLAG_SELECTABLE | OA_UNIT_FLAG_POSITION_DIRTY;
// Unit.flags the type's standing orders, cloak and build pages replace. The
// spawn also clears the collision marks.
constexpr uint32_t type_state_flags = OA_UNIT_FLAG_CLOAK_RUNNING | OA_UNIT_FLAG_MOVE_ORDER_MASK |
                                      OA_UNIT_FLAG_FIRE_ORDER_MASK | OA_UNIT_FLAG_BUILD_MENU |
                                      OA_UNIT_FLAG_BUILD_PAGE_MASK | OA_UNIT_FLAG_COLLISION_OTHER |
                                      OA_UNIT_FLAG_COLLISION_SELF;
// A type with two or more build pages opens on the build menu at page 1.
constexpr uint32_t build_menu_first_page =
    OA_UNIT_FLAG_BUILD_MENU | (1u << OA_UNIT_FLAG_BUILD_PAGE_SHIFT);

// Returns flags with bit set when on and cleared otherwise.
constexpr uint32_t with_flag(uint32_t flags, uint32_t bit, bool on) noexcept {
    return (flags & ~bit) | (on ? bit : 0u);
}

uint32_t arithmetic_shift20(uint32_t value) {
    const auto high = value >> 20;
    return (value & 0x80000000u) ? high | 0xfffff000u : high;
}

// Callbacks may replace the unit's type; the game reloads it from the unit.
// Null when a callback left the unit without one.
const oa::UnitDef* current_def(oa::World& w, const oa::Unit& u) noexcept {
    return oa::world_unit_def_of(&w, &u);
}

// The runtime type of the unit's current type, or null when it has none in
// the tables.
Type* current_type(oa::World& w, Tables& tables, const oa::Unit& u) noexcept {
    const auto* def = current_def(w, u);
    if (!def)
        return nullptr;
    const auto index = oa::world_unit_def_ref(&w, def) - 1u;
    return index < tables.types.size() ? &tables.types[index] : nullptr;
}

// The unit's slot asset record. create checks that the asset table covers the
// unit pool before it claims a slot.
SlotAssets& assets(oa::World& w, Tables& tables, const oa::Unit& u) noexcept {
    return tables.assets[oa::world_unit_slot(&w, &u)];
}

oa::oa_fixed fixed_word(uint32_t word) noexcept {
    return static_cast<oa::oa_fixed>(word);
}

// Signed high word of a 16.16 value.
int32_t high_word(oa::oa_fixed value) noexcept {
    return static_cast<int16_t>(static_cast<uint32_t>(value) >> 16);
}

// Sea occupy code of a unit created wholly under the sea, and of any other.
constexpr uint8_t created_submerged_code = 3;
constexpr uint8_t created_code = 0;

// Clears every reuse tick, as a creation or a death at game tick 0 does.
void clear_reuse_ticks(const oa::World& w, SpawnRules& rules) noexcept {
    if (w.game.tick == 0)
        for (auto& tick : rules.reuse_ticks)
            tick = 0;
}

// Whether the place at an offset of a player's range may be taken by a local
// creation: always without reuse ticks, else once the game tick has reached
// the place's reuse tick.
bool place_reusable(
    const oa::World& w, const SpawnRules& rules, uint8_t player, uint32_t offset
) noexcept {
    if (rules.reuse_ticks.empty())
        return true;
    const auto index = static_cast<std::size_t>(player) * w.game.units_per_player + offset;
    if (offset >= w.game.units_per_player || index >= rules.reuse_ticks.size())
        return true;
    return static_cast<int32_t>(w.game.tick) >= rules.reuse_ticks[index];
}
} // namespace

void load_unit_def(const Type& t, oa::UnitDef& d) noexcept {
    d.flags = t.simulation.flags;
    d.abilities = t.simulation.abilities;
    d.max_damage = t.simulation.maximum_health;
    d.heal_time = t.simulation.heal_time;
    d.water_line = std::bit_cast<int8_t>(t.simulation.waterline_offset);
    d.default_mission_type = std::bit_cast<int8_t>(t.simulation.default_mission_type);
    d.footprint_x = t.footprint_x;
    d.footprint_z = t.footprint_z;
    d.player_limit = t.player_limit;
    d.build_angle = std::bit_cast<int16_t>(t.build_angle);
    d.gui_page_count = t.gui_page_count;
    d.bm_code = std::bit_cast<int8_t>(t.bm_code);
}

SpawnFault
initialize_numeric(oa::World& w, oa::Unit& u, const Request& r, Host& h, const SpawnRules& rules) {
    const auto* owner = oa::world_unit_owner(&w, &u);
    if (!owner)
        return SpawnFault::no_owner;
    if (r.type >= w.unit_def_count)
        return SpawnFault::type_outside_table;
    const auto& t = w.unit_defs[r.type];
    u.flags |= OA_UNIT_FLAG_LIVE;
    u.def = oa::oa_ref_from_index(r.type);
    u.flags =
        with_flag(u.flags & ~OA_UNIT_FLAG_DEATH_PENDING, OA_UNIT_FLAG_BUILDING, t.bm_code == 0);
    // An east or west facing swaps the footprint's width and depth.
    const bool turned = (r.facing & 1) != 0;
    const auto footprint_x = turned ? t.footprint_z : t.footprint_x;
    const auto footprint_z = turned ? t.footprint_x : t.footprint_z;
    u.footprint_x = footprint_x;
    u.footprint_z = footprint_z;
    u.flags =
        with_flag(u.flags, OA_UNIT_FLAG_HAS_WEAPONS, (t.flags & OA_UNIT_DEF_FLAG_HAS_WEAPONS) != 0);
    u.flags2 =
        with_flag(u.flags2, OA_UNIT_FLAG2_Z_BUFFER, (t.flags & OA_UNIT_DEF_FLAG_Z_BUFFER) != 0);
    u.flags =
        with_flag(u.flags, OA_UNIT_FLAG_AIR_BASE, (t.flags & OA_UNIT_DEF_FLAG_IS_AIRBASE) != 0);
    if (!r.finished) {
        u.cleared_on_unfinished_spawn = 0;
        u.health = 0;
        u.build_remaining = 1.0F;
    } else {
        u.build_remaining = 0.0F;
        u.health = std::bit_cast<int16_t>(static_cast<uint16_t>(t.max_damage));
    }
    u.health_percent = 0;
    u.previous_health_percent = 0;
    u.position.x = fixed_word(r.position[0]);
    u.flags = (u.flags & ~spawn_cleared_flags) | spawn_set_flags;
    u.position.y = fixed_word(r.position[1]);
    u.state_flags = 0;
    u.position.z = fixed_word(r.position[2]);
    u.build_flags &= static_cast<uint8_t>(~OA_UNIT_BUILD_SCRIPT_MASK);
    u.decloak_until_tick = 0;
    u.pitch = 0;
    // Wrapping arithmetic, an arithmetic shift by 20, then each grid coordinate narrows.
    const auto grid_x = arithmetic_shift20(
        r.position[0] - static_cast<uint32_t>(static_cast<int32_t>(footprint_x)) * 0x80000u +
        0x80000u
    );
    const auto grid_z = arithmetic_shift20(
        r.position[2] - static_cast<uint32_t>(static_cast<int32_t>(footprint_z)) * 0x80000u +
        0x80000u
    );
    u.cell_x = std::bit_cast<int16_t>(static_cast<uint16_t>(grid_x));
    u.cell_z = std::bit_cast<int16_t>(static_cast<uint16_t>(grid_z));
    // Heading: a random value below the type's build angle, less 0x8000 and less
    // half the build angle, the half taken from the type the unit holds by then.
    const auto angle = h.random_bounded(static_cast<uint16_t>(t.build_angle));
    u.damage_countdown = 0;
    u.bank = 0;
    u.sight_center_x = 0;
    const auto* drawn_def = current_def(w, u);
    if (!drawn_def)
        return SpawnFault::type_removed;
    u.heading = static_cast<uint16_t>(
        angle + 0xffff8000u - (static_cast<uint16_t>(drawn_def->build_angle) >> 1)
    );
    u.sight_center_z = 0;
    u.capture_cooldown = 0;
    u.flags = with_flag(
        u.flags & ~OA_UNIT_FLAG_RADAR_CONTACT,
        OA_UNIT_FLAG_VIEWPOINT_OWNED,
        owner->index == w.game.viewpoint_player
    );
    for (uint32_t i = 0; i < 3; ++i) {
        h.init_weapon_target(u, i);
        h.reset_weapon_targets(u, static_cast<uint8_t>(i));
    }
    // The type is reloaded after the callbacks.
    const auto* reset_def = u.def ? current_def(w, u) : nullptr;
    if (!reset_def)
        return SpawnFault::type_removed;
    if (rules.start_submerged) {
        const auto sea_word = static_cast<int16_t>(
            static_cast<uint16_t>(w.game.sea_level | (w.game.debug_overlay << 8))
        );
        u.last_occupy_code[0] =
            high_word(reset_def->model_height) + high_word(u.position.y) < sea_word
                ? created_submerged_code
                : created_code;
    }
    const auto definition_flags = reset_def->flags;
    const auto flags_before_reset = u.flags;
    u.events = 0;
    u.veteran_level = 0;
    u.last_attacker_id = 0;
    u.last_attacker_owner = no_player;
    const auto standing_move =
        ((definition_flags & OA_UNIT_DEF_FLAG_MOVE_ORDER_MASK) << OA_UNIT_FLAG_MOVE_ORDER_SHIFT);
    const auto standing_fire =
        ((definition_flags & OA_UNIT_DEF_FLAG_FIRE_ORDER_MASK) >> OA_UNIT_DEF_FLAG_FIRE_ORDER_SHIFT)
        << OA_UNIT_FLAG_FIRE_ORDER_SHIFT;
    const auto cloak =
        (definition_flags & OA_UNIT_DEF_FLAG_INIT_CLOAKED) != 0 ? OA_UNIT_FLAG_CLOAK_RUNNING : 0u;
    const auto build_menu = reset_def->gui_page_count < 2 ? 0u : build_menu_first_page;
    u.flags = cloak | standing_fire | standing_move | build_menu |
              (flags_before_reset & ~type_state_flags);
    u.sight_band = 0;
    h.init_unit_economy(u, u.owner_index);
    u.attach_piece = 0xff;
    u.bob_phase = static_cast<uint16_t>(h.random_bounded(0x10000));
    h.assign_squad(u, 0);
    return SpawnFault::none;
}

SpawnFault attach_model_script(oa::World& w, Tables& tables, oa::Unit& u, Host& h) {
    if (u.type_index >= tables.types.size() || oa::world_unit_slot(&w, &u) >= tables.assets.size())
        return SpawnFault::type_outside_table;
    const auto& t = tables.types[u.type_index];
    auto& a = assets(w, tables, u);
    if (!t.cob) {
        a.script_instance = 0;
        u.script = 0;
        a.model_instance = h.create_model_instance(t.model);
        if (!a.model_instance)
            return SpawnFault::model_allocation_failed;
        h.model_owner(u);
    } else {
        a.script_instance = h.allocate_script();
        if (!a.script_instance)
            return SpawnFault::script_allocation_failed;
        u.script = 1;
        const auto* script_type = current_type(w, tables, u);
        if (!script_type)
            return SpawnFault::type_removed;
        h.load_script_state(a.script_instance, script_type->cob);
        script_type = current_type(w, tables, u);
        if (!script_type)
            return SpawnFault::type_removed;
        a.model_instance = h.create_scripted_model(t.model, script_type->cob, u);
        if (!a.model_instance)
            return SpawnFault::scripted_model_allocation_failed;
        h.bind_script_model(a.script_instance, a.model_instance);
        h.call_script_create(a.script_instance);
    }
    h.model_reset(u);
    return SpawnFault::none;
}

std::size_t unit_pool_size(uint16_t limit) noexcept {
    const auto total = static_cast<std::size_t>(limit) * 10 + 1;
    if (limit == 0 || total > 65535)
        return 0;
    return total;
}

std::size_t unit_pool_size(uint16_t limit, uint16_t player_mask) noexcept {
    if (limit == 0 || player_mask == 0 || (player_mask & ~player_slot_mask) != 0)
        return 0;
    const auto total = static_cast<std::size_t>(limit) * std::popcount(player_mask) + 1;
    return total <= wire_unit_slot_capacity ? total : 0;
}

bool init_unit_pool(oa::World& w, uint16_t limit) noexcept {
    const auto total = unit_pool_size(limit);
    if (total == 0 || !w.unit_defs || w.unit_def_count == 0 || !w.units ||
        w.unit_slot_count != total)
        return false;
    w.game.cycle_unit_id = 0;
    w.game.periodic_flags &= static_cast<uint8_t>(~periodic_enabled);
    w.game.units_per_player = limit;
    w.game.unit_slot_count = static_cast<uint16_t>(total);
    for (std::size_t i = 0; i < total; ++i) {
        w.units[i] = {};
        w.units[i].id = static_cast<uint16_t>(i);
        w.units[i].def = oa::oa_ref_from_index(0);
    }
    w.units[0].owner_index = 0xff;
    for (uint32_t player = 0; player < OA_PLAYER_COUNT; ++player) {
        const auto first = player * limit + 1u;
        auto& owner = w.game.players[player];
        owner.first_unit = oa::oa_ref_from_index(first);
        owner.last_unit = oa::oa_ref_from_index(first + limit - 1u);
        owner.base_unit_id = w.units[first].id;
        owner.last_unit_id = w.units[first + limit - 1u].id;
        for (uint32_t i = first; i < first + limit; ++i) {
            w.units[i].owner = oa::oa_ref_from_index(player);
            w.units[i].squad = -1;
            w.units[i].owner_index = owner.index;
        }
    }
    return true;
}

bool init_unit_pool(oa::World& w, uint16_t limit, uint16_t player_mask) noexcept {
    const auto total = unit_pool_size(limit, player_mask);
    if (total == 0 || !w.unit_defs || w.unit_def_count == 0 || !w.units ||
        w.unit_slot_count != total)
        return false;
    w.game.cycle_unit_id = 0;
    w.game.periodic_flags &= static_cast<uint8_t>(~periodic_enabled);
    w.game.units_per_player = limit;
    w.game.unit_slot_count = static_cast<uint16_t>(total);
    for (std::size_t i = 0; i < total; ++i) {
        w.units[i] = {};
        w.units[i].id = static_cast<uint16_t>(i);
        w.units[i].def = oa::oa_ref_from_index(0);
    }
    w.units[0].owner_index = 0xff;
    uint32_t first = 1;
    for (uint32_t player = 0; player < OA_PLAYER_COUNT; ++player) {
        auto& owner = w.game.players[player];
        owner.first_unit = 0;
        owner.last_unit = 0;
        owner.base_unit_id = 0;
        owner.last_unit_id = 0;
        if ((player_mask & (1u << player)) == 0)
            continue;
        const auto end = first + limit;
        owner.first_unit = oa::oa_ref_from_index(first);
        owner.last_unit = oa::oa_ref_from_index(end - 1);
        owner.base_unit_id = static_cast<uint16_t>(first);
        owner.last_unit_id = static_cast<uint16_t>(end - 1);
        for (auto i = first; i < end; ++i) {
            w.units[i].owner = oa::oa_ref_from_index(player);
            w.units[i].squad = -1;
            w.units[i].owner_index = owner.index;
        }
        first = end;
    }
    return true;
}

const char* spawn_fault_text(SpawnFault fault) noexcept {
    switch (fault) {
    case SpawnFault::none:
        return "no fault";
    case SpawnFault::player_outside_table:
        return "spawn player index";
    case SpawnFault::type_outside_table:
        return "spawn type index";
    case SpawnFault::asset_table_mismatch:
        return "spawn asset table does not match unit pool";
    case SpawnFault::player_slot_range:
        return "spawn player slot range";
    case SpawnFault::no_owner:
        return "spawn slot has no preassigned owner";
    case SpawnFault::type_removed:
        return "spawn callback removed unit type";
    case SpawnFault::model_allocation_failed:
        return "model allocation failed";
    case SpawnFault::script_allocation_failed:
        return "script allocation failed";
    case SpawnFault::scripted_model_allocation_failed:
        return "scripted model allocation failed";
    }
    return "unknown spawn fault";
}

oa::Unit* create(oa::World& w, Tables& tables, const Request& r, Host& h, SpawnFault* fault) {
    const auto stop = [fault](SpawnFault why) -> oa::Unit* {
        if (fault)
            *fault = why;
        return nullptr;
    };
    if (fault)
        *fault = SpawnFault::none;
    if (r.player >= OA_PLAYER_COUNT)
        return stop(SpawnFault::player_outside_table);
    if (!r.type)
        return nullptr;
    if (r.type >= w.unit_def_count || r.type >= tables.types.size())
        return stop(SpawnFault::type_outside_table);
    if (tables.assets.size() != w.unit_slot_count)
        return stop(SpawnFault::asset_table_mismatch);
    const auto& t = w.unit_defs[r.type];
    if (!(t.flags & OA_UNIT_DEF_FLAG_AVAILABLE))
        return nullptr;
    auto& p = w.game.players[r.player];
    uint32_t count = 0;
    auto* first = oa::world_player_units(&w, &p, &count);
    if (count == 0 && (p.first_unit || p.last_unit))
        return stop(SpawnFault::player_slot_range);
    if (t.player_limit != -1) {
        int32_t owned = 0;
        for (uint32_t i = 0; i < count; ++i)
            if (first[i].type_index == r.type)
                ++owned;
        if (t.player_limit <= owned)
            return nullptr;
    }
    oa::Unit* chosen = nullptr;
    const auto first_slot = count ? oa::world_unit_slot(&w, first) : 0u;
    clear_reuse_ticks(w, tables.rules);
    if (r.requested_slot) {
        const auto index = static_cast<uint32_t>(r.requested_slot);
        if (index < first_slot || index - first_slot >= count)
            return nullptr;
        if (w.units[index].type_index)
            return nullptr;
        chosen = &w.units[index];
    } else {
        for (uint32_t i = 0; i < count; ++i)
            if (!first[i].type_index && place_reusable(w, tables.rules, r.player, i)) {
                chosen = &first[i];
                break;
            }
    }
    if (!chosen)
        return nullptr;
    auto& u = *chosen;
    auto& a = assets(w, tables, u);
    a.weapon_slots_initialized.fill(true);
    u.type_index = r.type;
    if (const auto why = initialize_numeric(w, u, r, h, tables.rules); why != SpawnFault::none)
        return stop(why);
    if (const auto why = attach_model_script(w, tables, u, h); why != SpawnFault::none)
        return stop(why);
    h.initialize_weapons(u);
    h.initialize_extraction_rate(u);
    if (t.bm_code == 1) {
        a.movement_object = h.create_movement(u);
        // A null movement object is allowed and leaves Unit.movement clear.
        u.movement = a.movement_object != 0;
        // UnitDef.build_angle is written over Unit.heading after the movement object exists.
        const auto* moved_def = current_def(w, u);
        if (!moved_def)
            return stop(SpawnFault::type_removed);
        u.heading = static_cast<uint16_t>(moved_def->build_angle);
    }
    u.flags = ((u.flags ^ r.state) & OA_UNIT_FLAG_OCCUPANCY_MASK) ^ u.flags;
    h.fit_spawn_height(u);
    // A facing joins the heading before the unit takes its place, which the
    // occupancy and the other players read it from.
    u.heading = static_cast<uint16_t>(u.heading + (static_cast<uint32_t>(r.facing & 3) << 14));
    h.register_occupancy(u);
    h.notify_created(u);
    if (r.finished) {
        if (!t.bm_code)
            h.notify_finished(u);
        const auto* finished_def = u.def ? current_def(w, u) : nullptr;
        if (!finished_def)
            return stop(SpawnFault::type_removed);
        const auto flags = finished_def->flags;
        if (flags & OA_UNIT_DEF_FLAG_ACTIVATE_WHEN_BUILT)
            h.set_activation(u, true, true);
        // A finished unit whose type is a feature is marked to die at once,
        // dismissed (damage kind 7): it leaves its corpse and counts no kill.
        if (flags & OA_UNIT_DEF_FLAG_IS_FEATURE) {
            u.flags |= OA_UNIT_FLAG_DEATH_PENDING;
            u.damage_kind = 7;
        }
    }
    h.update_sight(u);
    ++p.unit_count;
    ++p.units_created;
    h.notify_scenario_created(u);
    return chosen;
}

void record_slot_death(oa::World& w, Tables& tables, const oa::Unit& u) noexcept {
    auto& rules = tables.rules;
    if (rules.reuse_ticks.empty())
        return;
    clear_reuse_ticks(w, rules);
    const auto* owner = oa::world_unit_owner(&w, &u);
    if (owner == nullptr)
        return;
    const uint32_t row = (u.capture_cooldown >> 8) & 0xffffu;
    if (row >= OA_PLAYER_COUNT)
        return;
    const auto place = static_cast<uint32_t>(
        static_cast<int32_t>(static_cast<int16_t>(u.id)) -
        static_cast<int32_t>(static_cast<int16_t>(owner->base_unit_id))
    );
    const auto per_player = static_cast<uint32_t>(w.game.units_per_player);
    if (place >= per_player)
        return;
    const auto index = static_cast<std::size_t>(row) * per_player + place;
    if (index >= rules.reuse_ticks.size())
        return;
    rules.reuse_ticks[index] =
        static_cast<int32_t>(w.game.tick + static_cast<uint32_t>(rules.reuse_delay_ticks));
}

int32_t count_start_positions(std::span<const StartMarker> markers) noexcept {
    int32_t count = 0;
    for (const auto& marker : markers)
        if (marker.kind == 1)
            ++count;
    return count;
}

bool start_position(
    std::span<const StartMarker> markers, int32_t index, std::array<uint32_t, 3>& output
) {
    for (const auto& marker : markers)
        if (marker.kind == 1 && marker.index == index) {
            output = {
                static_cast<uint32_t>(static_cast<int32_t>(marker.x)) << 16,
                0,
                static_cast<uint32_t>(static_cast<int32_t>(marker.z)) << 16
            };
            return true;
        }
    return false;
}

void grant_start_storage(oa::Player& player, int32_t metal, int32_t energy) noexcept {
    constexpr int32_t storage_floor = 200;
    player.resource_flags |= 1;
    player.shared_energy_storage =
        static_cast<float>(energy < storage_floor ? storage_floor : energy);
    player.shared_metal_storage = static_cast<float>(metal < storage_floor ? storage_floor : metal);
}

StartResult spawn_player_commander(
    oa::World& w,
    Tables& tables,
    uint8_t player,
    const PlayerSetup& setup,
    std::span<const StartMarker> markers,
    int32_t index,
    uint8_t local_player,
    int32_t viewport_width,
    int32_t viewport_height,
    Host& h,
    StartHost& start,
    SpawnFault* fault
) {
    if (fault)
        *fault = SpawnFault::none;
    if (player >= OA_PLAYER_COUNT || player >= tables.setups.size()) {
        if (fault)
            *fault = SpawnFault::player_outside_table;
        return {};
    }
    auto& p = w.game.players[player];
    auto& stored = tables.setups[player];
    stored.side = setup.side;
    stored.color = setup.color;
    grant_start_storage(p, setup.metal, setup.energy);
    Request request;
    request.player = player;
    request.finished = true;
    request.state = ground_occupancy_state;
    if (!start_position(markers, index, request.position)) {
        start.report_missing_start_position(index);
        // No commander without a start position; 3.1c can carry on after the report.
        return {};
    }
    request.type = start.commander_type_for_side(stored.side);
    auto* result = create(w, tables, request, h, fault);
    if (player == local_player) {
        const auto x = std::bit_cast<int16_t>(static_cast<uint16_t>(request.position[0] >> 16));
        const auto z = std::bit_cast<int16_t>(static_cast<uint16_t>(request.position[2] >> 16));
        const auto camera_x = static_cast<uint32_t>(static_cast<int32_t>(x)) -
                              static_cast<uint32_t>(viewport_width / 2);
        const auto camera_z = static_cast<uint32_t>(static_cast<int32_t>(z)) -
                              static_cast<uint32_t>(viewport_height / 2);
        start.set_camera_position(
            std::bit_cast<int32_t>(camera_x), std::bit_cast<int32_t>(camera_z), 0
        );
    }
    return {true, result};
}

int32_t attached_child_count(const oa::World& w, const oa::Unit& unit) {
    const auto self = oa::world_unit_ref(&w, &unit);
    int32_t count = 0;
    uint32_t seen = 0;
    for (const auto* child = oa::world_unit(&w, unit.attach_first_child); child != nullptr;
         child = oa::world_unit(&w, child->attach_next)) {
        // The slot count is a 16-bit word, so a longer chain must cycle.
        if (++seen > 65535)
            break;
        if (child->attach_parent == self)
            ++count;
    }
    return count;
}

int32_t average_plot_height(
    std::span<const uint32_t, 3> position,
    std::span<const sim::spatial_state::Plot> plots,
    int32_t map_width,
    int32_t map_height
) {
    const auto high_word = [](uint32_t word) noexcept {
        return static_cast<int16_t>(static_cast<uint16_t>(word >> 16));
    };
    // Signed division by 16 truncates toward zero, including negatives.
    const auto cell_x = static_cast<int32_t>(high_word(position[0])) / 16;
    const auto cell_z = static_cast<int32_t>(high_word(position[2])) / 16;
    const auto index =
        sim::spatial_state::plot_index_at(cell_x, cell_z, plots, map_width, map_height);
    if (!index)
        return -1;
    const auto& plot = plots[*index];
    return (static_cast<int32_t>(plot.low_height) + static_cast<int32_t>(plot.high_height)) >> 1;
}

bool player_pay_energy(oa::Player& player, float& requested, float amount) noexcept {
    const bool accepted = amount <= player.energy;
    if (accepted) {
        player.energy -= amount;
        requested += amount;
    }
    return accepted;
}

bool player_pay_metal(oa::Player& player, float& requested, float amount) noexcept {
    const bool accepted = amount <= player.metal;
    if (accepted) {
        player.metal -= amount;
        requested += amount;
    }
    return accepted;
}

bool player_pay_resources(
    oa::Player& player, float& energy_requested, float& metal_requested, float energy, float metal
) noexcept {
    if (!(energy <= player.energy) || !(metal <= player.metal))
        return false;
    (void)player_pay_energy(player, energy_requested, energy);
    (void)player_pay_metal(player, metal_requested, metal);
    return true;
}
} // namespace oa::sim::unit_spawn
