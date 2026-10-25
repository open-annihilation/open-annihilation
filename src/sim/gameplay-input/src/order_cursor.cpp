// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/gameplay_input/order_cursor.hpp"

#include "oa/sim/feature_runtime.hpp"
#include "oa/sim/gameplay_input/input.hpp"
#include "oa/sim/weapon_execution/projectile_contact.hpp"

#include <cmath>

namespace oa::sim::gameplay_input {
namespace {
/// Returns the [Interface] preference: interface_left_click or interface_right_click.
int32_t interface_type(const Game& game) noexcept {
    return game.interface_type;
}

// The compare against 0.0 treats an unordered NaN as equal.
bool zero_or_unordered(float value) noexcept {
    return value == 0.0F || std::isnan(value);
}

int16_t high_word(int32_t fixed) noexcept {
    return static_cast<int16_t>(static_cast<uint32_t>(fixed) >> 16U);
}

uint8_t occupancy(const Unit& unit) noexcept {
    return static_cast<uint8_t>(unit.flags & OA_UNIT_FLAG_OCCUPANCY_MASK);
}

// Occupancy value of a flying unit (the air layer); a unit a transport
// carries is in layer 0.
constexpr uint8_t occupancy_airborne = 2;

bool is_local(const World& world, const Unit& unit) noexcept {
    return unit.owner_index == world.game.local_player_index;
}

bool has_movement_object(
    const World& world, const Unit& unit, const OrderCursorHooks& hooks
) noexcept {
    return hooks.movement_object != nullptr ? hooks.movement_object(hooks.context, world, unit)
                                            : unit.movement != 0;
}

// Units attached below `unit` (the transport's cargo count).
uint32_t child_count(const World& world, const Unit& unit) noexcept {
    uint32_t count = 0;
    const Unit* child = world_unit(&world, unit.attach_first_child);
    while (child != nullptr && count <= world.unit_slot_count) {
        ++count;
        child = world_unit(&world, child->attach_next);
    }
    return count;
}

// A 16.16 map coordinate's cell (an arithmetic shift).
int32_t map_cell(int32_t fixed) noexcept {
    return fixed >> 20;
}

// The highest map height, 16.16: the highest a feature at rest stands.
constexpr int64_t highest_map_height = int64_t{255} << 16;
// MapPlot.feature of a footprint cell after its origin; MapPlot.feature_record
// then holds the rows back to the origin in its low byte, the columns in its
// high byte.
constexpr uint16_t footprint_continuation = 0xfffe;

// A reclaimable feature on mapped ground, in sight or under the fog.
bool reclaimable_feature_mapped(
    const World& world, const FixedVec3& position, const OrderCursorHooks& hooks
) noexcept {
    if (hooks.position_mapped == nullptr || !hooks.position_mapped(hooks.context, world, position))
        return false;
    if (hooks.feature_at == nullptr)
        return false;
    const FeatureDef* feature = hooks.feature_at(hooks.context, world, position);
    return feature != nullptr && (feature->flags & OA_FEATURE_FLAG_RECLAIMABLE) != 0;
}

// Whether the rules keep an aircraft of this type off repair pads
// (air.no-repair-retreat-flag): the Move command over a pad then neither
// shows the pad cursor nor lands on it, nor does a right press on it in the
// right-click interface; the pointer falls through to the load and guard
// tests.
bool kept_off_pads(const UnitDef& actor_def, const OrderCursorHooks& hooks) noexcept {
    using Flag = data::match_rules::AirNoRepairRetreatFlagFlag;
    return hooks.rules.rules().air.no_repair_retreat_flag.flag == Flag::cantbetransported &&
           (actor_def.abilities & OA_UNIT_DEF_ABILITY_CANT_BE_TRANSPORTED) != 0;
}

OrderCursor load_cursor(const UnitDef& actor_def) noexcept {
    return (actor_def.flags & OA_UNIT_DEF_FLAG_CAN_FLY) != 0 ? OrderCursor::load_by_air
                                                             : OrderCursor::load;
}
} // namespace

const FeatureDef* feature_at_position(const World& world, const FixedVec3& position) noexcept {
    const MapPlot* plot = world_plot(&world, map_cell(position.x), map_cell(position.z));
    return plot != nullptr ? sim::weapon_execution::plot_feature_def(world, *plot) : nullptr;
}

std::optional<DrawnFeature> drawn_feature(const World& world, const FixedVec3& ground) noexcept {
    if (world.feature_defs == nullptr)
        return std::nullopt;
    // The ground point's screen row, as the map Z a point at height 0 drawn
    // on it has.
    const int64_t row = int64_t{ground.z} - (int64_t{ground.y} >> 1);
    const int32_t column = map_cell(ground.x);
    const int64_t nearest = (row + (highest_map_height >> 1)) >> 20;
    const int64_t farthest = row >> 20;
    for (int64_t cell_z = nearest; cell_z >= farthest; --cell_z) {
        const MapPlot* plot = world_plot(&world, column, static_cast<int32_t>(cell_z));
        if (plot == nullptr)
            continue;
        int32_t origin_x = column;
        auto origin_z = static_cast<int32_t>(cell_z);
        uint16_t word = plot->feature;
        if (word == footprint_continuation) {
            origin_z -= static_cast<int32_t>(plot->feature_record & 0xffU);
            origin_x -= static_cast<int32_t>(plot->feature_record >> 8U);
            const MapPlot* origin = world_plot(&world, origin_x, origin_z);
            if (origin == nullptr)
                continue;
            word = origin->feature;
        }
        if (word >= OA_PLOT_FEATURE_RESERVED || word >= world.feature_def_count ||
            static_cast<int32_t>(word) >= world.game.feature_def_count)
            continue;
        const oa_fixed height = feature_runtime::feature_drawn_height(world, origin_x, origin_z);
        const int64_t stands_z = row + (int64_t{height} >> 1);
        if ((stands_z >> 20) != cell_z)
            continue;
        return DrawnFeature{
            &world.feature_defs[word], {ground.x, height, static_cast<oa_fixed>(stands_z)}
        };
    }
    return std::nullopt;
}

OrderCommand pointer_command(const Game& game) noexcept {
    return static_cast<OrderCommand>(game.pointer_command);
}

void set_pointer_command(Game& game, OrderCommand command) noexcept {
    game.pointer_command = static_cast<uint8_t>(command);
}

uint8_t pointer_flags(const Game& game) noexcept {
    return game.pointer_flags;
}

void set_pointer_flags(Game& game, uint8_t flags) noexcept {
    game.pointer_flags = flags;
}

void set_pointer_area(Game& game, bool in_radar, bool in_view) noexcept {
    uint8_t flags = pointer_flags(game);
    if (in_radar && (flags & pointer_box_drag) == 0)
        flags = static_cast<uint8_t>((flags | pointer_over_radar) & ~pointer_over_view);
    else
        flags = static_cast<uint8_t>(
            (flags & ~(pointer_over_radar | pointer_over_view)) | (in_view ? pointer_over_view : 0)
        );
    flags = static_cast<uint8_t>(flags & ~pointer_over_map);
    if ((flags & (pointer_over_radar | pointer_over_view)) != 0)
        flags |= pointer_over_map;
    set_pointer_flags(game, flags);
}

RightPress right_press(Game& game) noexcept {
    if (pointer_command(game) != OrderCommand::default_order)
        return RightPress::cancel_command;
    const uint8_t flags = pointer_flags(game);
    if (interface_type(game) != interface_left_click) {
        if ((flags & pointer_over_map) == 0)
            return RightPress::none;
        // Over the radar the view goes to the point instead of the default
        // order 3.1c gives there.
        return (flags & pointer_over_radar) != 0 ? RightPress::radar_jump
                                                 : RightPress::default_order;
    }
    if ((flags & pointer_over_view) != 0)
        return (game.pointer_state[2] & pointer_key_control) != 0 ? RightPress::mouse_look
                                                                  : RightPress::clear_selection;
    if ((flags & pointer_over_radar) == 0)
        return RightPress::none;
    set_pointer_flags(game, flags | pointer_radar_scroll);
    return RightPress::radar_scroll;
}

bool start_left_radar_scroll(Game& game) noexcept {
    const uint8_t flags = pointer_flags(game);
    if ((flags & pointer_radar_scroll) != 0 || game.mouse_look_active != 0 ||
        pointer_command(game) != OrderCommand::default_order ||
        (flags & (pointer_box_drag | pointer_over_view)) != 0 ||
        interface_type(game) != interface_right_click || (flags & pointer_over_radar) == 0)
        return false;
    set_pointer_flags(game, flags | pointer_radar_scroll);
    return true;
}

bool end_radar_scroll(Game& game, bool right_button) noexcept {
    const uint8_t flags = pointer_flags(game);
    if ((flags & pointer_radar_scroll) == 0 ||
        right_button != (interface_type(game) == interface_left_click))
        return false;
    set_pointer_flags(game, static_cast<uint8_t>(flags & ~pointer_radar_scroll));
    return true;
}

FixedVec3 pointer_position(const Game& game) noexcept {
    return game.cursor_position;
}

void set_pointer_position(Game& game, const FixedVec3& position) noexcept {
    game.cursor_position = position;
}

bool unit_accepts_order(const World& world, const Unit& unit) noexcept {
    if ((unit.flags & OA_UNIT_FLAG_SELECTABLE) == 0 || !zero_or_unordered(unit.build_remaining) ||
        unit.capture_cooldown != 0)
        return false;
    if (unit.attach_parent == 0)
        return true;
    const Unit* carrier = world_unit(&world, unit.attach_parent);
    return carrier != nullptr && (carrier->flags & OA_UNIT_FLAG_AIR_BASE) != 0;
}

bool can_reclaim_unit(const World& world, const Unit& actor, const Unit& target) noexcept {
    const UnitDef* actor_def = world_unit_def_of(&world, &actor);
    const UnitDef* target_def = world_unit_def_of(&world, &target);
    if (actor_def == nullptr || target_def == nullptr)
        return false;
    return (actor_def->abilities & OA_UNIT_DEF_ABILITY_CAN_RECLAMATE) != 0 &&
           occupancy(target) != occupancy_airborne &&
           (target_def->abilities & OA_UNIT_DEF_ABILITY_CAN_CAPTURE) == 0;
}

bool can_repair_unit(const World& world, const Unit& actor, const Unit& target) noexcept {
    const UnitDef* actor_def = world_unit_def_of(&world, &actor);
    const UnitDef* target_def = world_unit_def_of(&world, &target);
    if (actor_def == nullptr || target_def == nullptr ||
        (actor_def->abilities & OA_UNIT_DEF_ABILITY_CAN_REPAIR) == 0 ||
        static_cast<int32_t>(target.health) == static_cast<int32_t>(target_def->max_damage) ||
        occupancy(target) == occupancy_airborne)
        return false;
    const int32_t sea_level = world.game.sea_level;
    const int32_t top = high_word(target_def->model_height) + high_word(target.position.y);
    const bool flies = (actor_def->flags & OA_UNIT_DEF_FLAG_CAN_FLY) != 0;
    if (flies && (actor_def->flags & OA_UNIT_DEF_FLAG_AMPHIBIOUS) == 0 && top < sea_level)
        return false;
    return flies || sea_level - actor_def->max_water_depth <= top;
}

bool can_load_unit(const World& world, const Unit& actor, const Unit& target) noexcept {
    const UnitDef* actor_def = world_unit_def_of(&world, &actor);
    const UnitDef* target_def = world_unit_def_of(&world, &target);
    if (actor_def == nullptr || target_def == nullptr)
        return false;
    if ((target_def->abilities & OA_UNIT_DEF_ABILITY_CANT_BE_TRANSPORTED) != 0 ||
        (actor_def->abilities & OA_UNIT_DEF_ABILITY_CAN_LOAD) == 0)
        return false;
    const auto capacity = static_cast<uint8_t>(actor_def->transport_capacity);
    if (child_count(world, actor) >= capacity || target.movement == 0)
        return false;
    const auto size = static_cast<uint8_t>(actor_def->transport_size);
    if (target_def->footprint_x > static_cast<int16_t>(size) ||
        occupancy(target) == occupancy_airborne)
        return false;
    if ((actor_def->flags & OA_UNIT_DEF_FLAG_CAN_FLY) == 0 && target_def->min_water_depth >= 0)
        return false;
    const auto sea_level_fixed =
        static_cast<int32_t>(static_cast<uint32_t>(world.game.sea_level) << 16U);
    const auto top = static_cast<int32_t>(
        static_cast<uint32_t>(target_def->model_height) + static_cast<uint32_t>(target.position.y)
    );
    return sea_level_fixed < top && zero_or_unordered(target.build_remaining);
}

OrderCursor order_cursor(
    const World& world,
    OrderCommand command,
    const Unit& actor,
    const Unit* target,
    const FixedVec3& position,
    const OrderCursorHooks& hooks
) noexcept {
    const UnitDef* def = world_unit_def_of(&world, &actor);
    if (def == nullptr)
        return OrderCursor::normal;
    const uint32_t abilities = def->abilities;
    bool allied = false, enemy = false;
    if (target != nullptr) {
        const Player* actor_owner = world_unit_owner(&world, &actor);
        const Player* target_owner = world_unit_owner(&world, target);
        if (actor_owner != nullptr && target_owner != nullptr &&
            target_owner->index < sizeof actor_owner->alliance &&
            actor_owner->alliance[target_owner->index] != 0)
            allied = true;
        else
            enemy = true;
    }
    const auto selectable_local = [&] {
        return target != nullptr && is_local(world, *target) && unit_accepts_order(world, *target);
    };
    // The default order re-enters as ATTACK or RECLAIM; at most two passes.
    for (;;) {
        switch (command) {
        case OrderCommand::default_order:
            if (interface_type(world.game) == interface_right_click) {
                if (selectable_local())
                    return OrderCursor::select;
                if (enemy)
                    return OrderCursor::enemy;
                if (allied)
                    return OrderCursor::friendly;
                if ((abilities & OA_UNIT_DEF_ABILITY_CAN_RESURRECT) != 0 &&
                    reclaimable_feature_mapped(world, position, hooks))
                    return OrderCursor::friendly;
                if ((abilities & OA_UNIT_DEF_ABILITY_CAN_RECLAMATE) != 0 &&
                    reclaimable_feature_mapped(world, position, hooks))
                    return OrderCursor::friendly;
                return OrderCursor::normal;
            }
            if ((abilities & OA_UNIT_DEF_ABILITY_CAN_ATTACK) != 0 && enemy) {
                command = OrderCommand::attack;
                continue;
            }
            if ((abilities & OA_UNIT_DEF_ABILITY_CAN_RECLAMATE) != 0 && enemy) {
                command = OrderCommand::reclaim;
                continue;
            }
            if (target != nullptr) {
                if (can_repair_unit(world, actor, *target) &&
                    !zero_or_unordered(target->build_remaining))
                    return OrderCursor::repair;
                if (selectable_local())
                    return OrderCursor::select;
            }
            if ((abilities & OA_UNIT_DEF_ABILITY_CAN_RESURRECT) != 0 &&
                reclaimable_feature_mapped(world, position, hooks))
                return OrderCursor::resurrect;
            if ((abilities & OA_UNIT_DEF_ABILITY_CAN_RECLAMATE) != 0 &&
                reclaimable_feature_mapped(world, position, hooks))
                return OrderCursor::reclaim;
            return (abilities & OA_UNIT_DEF_ABILITY_CAN_MOVE) != 0 ? OrderCursor::move
                                                                   : OrderCursor::normal;
        case OrderCommand::move:
            if ((abilities & OA_UNIT_DEF_ABILITY_CAN_MOVE) == 0)
                return OrderCursor::normal;
            if ((abilities & OA_UNIT_DEF_ABILITY_CAN_RESURRECT) != 0 &&
                reclaimable_feature_mapped(world, position, hooks))
                return OrderCursor::resurrect;
            if (target != nullptr && has_movement_object(world, actor, hooks)) {
                if ((abilities & OA_UNIT_DEF_ABILITY_CAN_CAPTURE) == 0) {
                    if (enemy && can_reclaim_unit(world, actor, *target))
                        return OrderCursor::reclaim;
                } else if (enemy) {
                    return OrderCursor::capture;
                }
                if (allied && can_repair_unit(world, actor, *target))
                    return OrderCursor::repair;
                const UnitDef* target_def = world_unit_def_of(&world, target);
                if ((def->flags & OA_UNIT_DEF_FLAG_CAN_FLY) != 0 && target_def != nullptr &&
                    (target_def->flags & OA_UNIT_DEF_FLAG_IS_AIRBASE) != 0 &&
                    !kept_off_pads(*def, hooks))
                    return hooks.pad_load_cursor ? OrderCursor::load : OrderCursor::unload;
                if (can_load_unit(world, actor, *target))
                    return load_cursor(*def);
                if ((abilities & OA_UNIT_DEF_ABILITY_CAN_GUARD) != 0 && allied)
                    return OrderCursor::guard;
            }
            return OrderCursor::move;
        case OrderCommand::attack: {
            if ((abilities & OA_UNIT_DEF_ABILITY_CAN_ATTACK) == 0)
                return OrderCursor::normal;
            const WeaponDef* primary = world_weapon_def(&world, def->weapon1);
            if (primary != nullptr && (primary->flags & OA_WEAPON_FLAG_DROPPED) != 0)
                return OrderCursor::attack_dropped;
            if (!has_movement_object(world, actor, hooks)) {
                if (target != nullptr) {
                    const bool reach = hooks.unit_in_range != nullptr &&
                                       hooks.unit_in_range(hooks.context, world, actor, *target);
                    return reach ? OrderCursor::attack : OrderCursor::attack_out_of_range;
                }
                if (hooks.position_in_range == nullptr ||
                    !hooks.position_in_range(hooks.context, world, actor, position))
                    return OrderCursor::attack_out_of_range;
                const WeaponDef* weapon = world_weapon_def(&world, actor.weapons[0].def);
                if (weapon != nullptr && (weapon->flags & OA_WEAPON_FLAG_TO_AIR_WEAPON) != 0)
                    return OrderCursor::attack_out_of_range;
            }
            return OrderCursor::attack;
        }
        case OrderCommand::blast: {
            if ((abilities & OA_UNIT_DEF_ABILITY_CAN_DGUN) == 0)
                return OrderCursor::normal;
            const WeaponDef* weapon = world_weapon_def(&world, actor.weapons[2].def);
            const Player* payer = world_player_ref(&world, actor.economy.player);
            if (weapon != nullptr && payer != nullptr && weapon->energy_per_shot <= payer->energy &&
                weapon->metal_per_shot <= payer->metal)
                return OrderCursor::attack;
            return OrderCursor::attack_out_of_range;
        }
        case OrderCommand::unload:
            return (abilities & OA_UNIT_DEF_ABILITY_CAN_LOAD) != 0 ? OrderCursor::unload
                                                                   : OrderCursor::normal;
        case OrderCommand::load:
            if (target != nullptr && can_load_unit(world, actor, *target))
                return load_cursor(*def);
            return OrderCursor::normal;
        case OrderCommand::guard: {
            if ((abilities & OA_UNIT_DEF_ABILITY_CAN_GUARD) == 0 || !allied)
                return OrderCursor::normal;
            const UnitDef* target_def = world_unit_def_of(&world, target);
            const bool actor_flies = (def->flags & OA_UNIT_DEF_FLAG_CAN_FLY) != 0;
            const bool target_flies =
                target_def != nullptr && (target_def->flags & OA_UNIT_DEF_FLAG_CAN_FLY) != 0;
            return actor_flies || !target_flies ? OrderCursor::guard : OrderCursor::normal;
        }
        case OrderCommand::repair:
            return target != nullptr && can_repair_unit(world, actor, *target)
                       ? OrderCursor::repair
                       : OrderCursor::normal;
        case OrderCommand::patrol:
            return (abilities & OA_UNIT_DEF_ABILITY_CAN_PATROL) != 0 ? OrderCursor::patrol
                                                                     : OrderCursor::normal;
        case OrderCommand::teleport:
            return OrderCursor::teleport;
        case OrderCommand::reclaim:
            if ((abilities & OA_UNIT_DEF_ABILITY_CAN_RECLAMATE) != 0 &&
                reclaimable_feature_mapped(world, position, hooks))
                return OrderCursor::reclaim;
            // With reclaim-command-any-unit the cursor over a unit is always
            // reclaim, so hovering it no longer tells which units refuse.
            if (target != nullptr && (hooks.rules.rules().orders.reclaim_command_any_unit.enabled ||
                                      can_reclaim_unit(world, actor, *target)))
                return OrderCursor::reclaim;
            return OrderCursor::normal;
        case OrderCommand::capture:
            if ((abilities & OA_UNIT_DEF_ABILITY_CAN_CAPTURE) != 0 && target != nullptr &&
                actor.owner != target->owner)
                return OrderCursor::capture;
            return OrderCursor::normal;
        case OrderCommand::build:
            return def->build_ids != 0 && has_movement_object(world, actor, hooks)
                       ? OrderCursor::build
                       : OrderCursor::normal;
        default:
            return OrderCursor::normal;
        }
    }
}

uint32_t collect_selected_units(const World& world, const Unit** out, uint32_t capacity) noexcept {
    const Player* local = world_player(&world, world.game.local_player_index);
    if (local == nullptr)
        return 0;
    const Unit* first = world_unit(&world, local->first_unit);
    const Unit* last = world_unit(&world, local->last_unit);
    uint32_t count = 0;
    for (const Unit* unit = first; unit != nullptr && last != nullptr && unit <= last; ++unit) {
        if ((unit->flags & OA_UNIT_FLAG_SELECTED) == 0)
            continue;
        if (count == capacity)
            break;
        out[count++] = unit;
    }
    return count;
}

OrderCursor selection_order_cursor(
    const World& world, OrderCommand command, const OrderCursorHooks& hooks
) noexcept {
    const FixedVec3 position = pointer_position(world.game);
    const Unit* cursor_unit =
        world.game.cursor_unit_id != 0 ? world_unit_at(&world, world.game.cursor_unit_id) : nullptr;
    OrderCursor best = OrderCursor::normal;
    bool any = false;
    const Player* local = world_player(&world, world.game.local_player_index);
    if (local != nullptr) {
        const Unit* first = world_unit(&world, local->first_unit);
        const Unit* last = world_unit(&world, local->last_unit);
        for (const Unit* unit = first; unit != nullptr && last != nullptr && unit <= last; ++unit) {
            if ((unit->flags & OA_UNIT_FLAG_SELECTED) == 0 || unit == cursor_unit)
                continue;
            any = true;
            const OrderCursor cursor =
                order_cursor(world, command, *unit, cursor_unit, position, hooks);
            if (static_cast<uint8_t>(cursor) < static_cast<uint8_t>(best))
                best = cursor;
        }
    }
    if (any)
        return best;
    if (command == OrderCommand::default_order && cursor_unit != nullptr &&
        is_local(world, *cursor_unit) && unit_accepts_order(world, *cursor_unit))
        return OrderCursor::select;
    return OrderCursor::normal;
}

bool pointer_cursor(
    const World& world, const OrderCursorHooks& hooks, OrderCursor* cursor
) noexcept {
    const uint8_t flags = pointer_flags(world.game);
    const OrderCommand command = pointer_command(world.game);
    const bool over_view = (flags & pointer_over_view) != 0;
    if (over_view && command == OrderCommand::build)
        return false;
    if (!over_view && (flags & pointer_over_radar) == 0)
        *cursor = OrderCursor::normal;
    else
        *cursor = selection_order_cursor(world, command, hooks);
    return true;
}

UnitOrder unit_order(
    const World& world,
    OrderCommand command,
    const Unit& actor,
    const Unit* target,
    const FixedVec3* position,
    const OrderCursorHooks& hooks
) noexcept {
    const UnitDef* def = world_unit_def_of(&world, &actor);
    if (def == nullptr)
        return UnitOrder::none;
    bool allied = false, enemy = false;
    if (target != nullptr) {
        if ((target->flags & OA_UNIT_FLAG_LIVE) == 0)
            return UnitOrder::none;
        enemy = true;
        const Player* actor_owner = world_unit_owner(&world, &actor);
        const Player* target_owner = world_unit_owner(&world, target);
        if (actor_owner != nullptr && target_owner != nullptr &&
            target_owner->index < sizeof actor_owner->alliance &&
            actor_owner->alliance[target_owner->index] != 0) {
            allied = true;
            enemy = false;
        }
    }
    const uint32_t abilities = def->abilities;
    const bool flies = (def->flags & OA_UNIT_DEF_FLAG_CAN_FLY) != 0;
    const auto by_air = [flies](UnitOrder ground, UnitOrder air) { return flies ? air : ground; };
    const UnitDef* target_def = target != nullptr ? world_unit_def_of(&world, target) : nullptr;
    const bool target_flies =
        target_def != nullptr && (target_def->flags & OA_UNIT_DEF_FLAG_CAN_FLY) != 0;
    const bool target_airbase =
        target_def != nullptr && (target_def->flags & OA_UNIT_DEF_FLAG_IS_AIRBASE) != 0;
    const bool target_unfinished = target != nullptr && !zero_or_unordered(target->build_remaining);
    const auto assistable = [&] {
        return target != nullptr && can_repair_unit(world, actor, *target);
    };
    const auto assist_order = [&] {
        return target_unfinished ? by_air(UnitOrder::help_build, UnitOrder::vtol_help_build)
                                 : by_air(UnitOrder::repair_unit, UnitOrder::vtol_repair_unit);
    };
    const auto feature_here = [&] {
        return position != nullptr && reclaimable_feature_mapped(world, *position, hooks);
    };
    const auto kamikaze = [&] {
        return (def->flags & OA_UNIT_DEF_FLAG_KAMIKAZE) != 0 ? UnitOrder::attack_kamikaze
                                                             : UnitOrder::none;
    };
    for (;;) {
        switch (command) {
        case OrderCommand::default_order:
            if ((abilities & OA_UNIT_DEF_ABILITY_CAN_ATTACK) != 0 && enemy) {
                command = OrderCommand::attack;
                continue;
            }
            if (interface_type(world.game) == interface_right_click) {
                if ((abilities & OA_UNIT_DEF_ABILITY_CAN_RECLAMATE) != 0 && enemy)
                    return by_air(UnitOrder::reclaim_unit, UnitOrder::vtol_reclaim_unit);
                if (allied && assistable())
                    return assist_order();
                if (flies && allied && target_airbase && !kept_off_pads(*def, hooks))
                    return UnitOrder::vtol_landing;
                if (target != nullptr && can_load_unit(world, actor, *target))
                    return by_air(UnitOrder::ground_pickup, UnitOrder::vtol_pickup);
                if ((abilities & OA_UNIT_DEF_ABILITY_CAN_GUARD) != 0 && allied)
                    return by_air(UnitOrder::follow_ground, UnitOrder::vtol_follow);
            } else {
                if ((abilities & OA_UNIT_DEF_ABILITY_CAN_RECLAMATE) != 0 && enemy) {
                    command = OrderCommand::reclaim;
                    continue;
                }
                if (target != nullptr) {
                    if (assistable() && target_unfinished) {
                        command = OrderCommand::repair;
                        continue;
                    }
                    if (is_local(world, *target) && unit_accepts_order(world, *target))
                        return UnitOrder::none;
                }
            }
            if ((abilities & OA_UNIT_DEF_ABILITY_CAN_RESURRECT) != 0 && feature_here())
                return UnitOrder::resurrect;
            if ((abilities & OA_UNIT_DEF_ABILITY_CAN_RECLAMATE) != 0 && feature_here())
                return by_air(UnitOrder::reclaim, UnitOrder::vtol_reclaim);
            if ((abilities & OA_UNIT_DEF_ABILITY_CAN_MOVE) == 0 ||
                !has_movement_object(world, actor, hooks))
                return UnitOrder::none;
            return by_air(UnitOrder::move_ground, UnitOrder::vtol_move);
        case OrderCommand::move:
            if ((abilities & OA_UNIT_DEF_ABILITY_CAN_MOVE) == 0)
                return UnitOrder::none;
            if (!has_movement_object(world, actor, hooks))
                return UnitOrder::qmove;
            if (target != nullptr) {
                if ((abilities & OA_UNIT_DEF_ABILITY_CAN_CAPTURE) != 0 && enemy)
                    return UnitOrder::capture;
                if ((abilities & OA_UNIT_DEF_ABILITY_CAN_RECLAMATE) != 0 && enemy)
                    return by_air(UnitOrder::reclaim_unit, UnitOrder::vtol_reclaim_unit);
                if (allied && assistable()) {
                    if (target_unfinished)
                        return by_air(UnitOrder::help_build, UnitOrder::vtol_help_build);
                    // Unsigned compare: negative health never counts as damaged.
                    if (static_cast<uint32_t>(static_cast<int32_t>(target->health)) <
                        static_cast<uint32_t>(target_def->max_damage))
                        return by_air(UnitOrder::repair_unit, UnitOrder::vtol_repair_unit);
                }
                if (flies && allied && target_airbase && !kept_off_pads(*def, hooks))
                    return UnitOrder::vtol_landing;
                if (can_load_unit(world, actor, *target))
                    return by_air(UnitOrder::ground_pickup, UnitOrder::vtol_pickup);
                if ((abilities & OA_UNIT_DEF_ABILITY_CAN_GUARD) != 0 && allied)
                    return by_air(UnitOrder::follow_ground, UnitOrder::vtol_follow);
            }
            return by_air(UnitOrder::move_ground, UnitOrder::vtol_move);
        case OrderCommand::attack: {
            if ((abilities & OA_UNIT_DEF_ABILITY_CAN_ATTACK) == 0)
                return UnitOrder::none;
            if ((actor.flags & OA_UNIT_FLAG_HAS_WEAPONS) == 0)
                return kamikaze();
            const WeaponDef* primary = world_weapon_def(&world, actor.weapons[0].def);
            const bool primary_to_air =
                primary != nullptr && (primary->flags & OA_WEAPON_FLAG_TO_AIR_WEAPON) != 0;
            const WeaponDef* type_primary = world_weapon_def(&world, def->weapon1);
            const bool dropped =
                type_primary != nullptr && (type_primary->flags & OA_WEAPON_FLAG_DROPPED) != 0;
            if (!enemy) {
                if (primary_to_air)
                    return UnitOrder::none;
                if (!flies)
                    return UnitOrder::suppress;
                return dropped ? UnitOrder::air_strike : UnitOrder::air_to_ground;
            }
            if (occupancy(*target) != occupancy_airborne && primary_to_air)
                return UnitOrder::none;
            const int32_t top = (target_def != nullptr ? high_word(target_def->model_height) : 0) +
                                high_word(target->position.y);
            const int32_t sea_level = world.game.sea_level;
            const bool primary_water =
                primary != nullptr && (primary->flags & OA_WEAPON_FLAG_WATER_WEAPON) != 0;
            const WeaponDef* secondary = world_weapon_def(&world, actor.weapons[1].def);
            const bool secondary_water = (actor.weapons[1].flags & OA_UNIT_WEAPON_ENABLED) != 0 &&
                                         secondary != nullptr &&
                                         (secondary->flags & OA_WEAPON_FLAG_WATER_WEAPON) != 0;
            if (top < sea_level && !primary_water && !secondary_water)
                return UnitOrder::none;
            // A primary weapon with the surface-fire key lets a hovering
            // unit's water weapons attack above sea level.
            const bool primary_surface_fire =
                primary != nullptr && hooks.rules.weapon(oa_weapon_def_id(primary)).surface_fire;
            if (sea_level <= top && (def->flags & OA_UNIT_DEF_FLAG_CAN_HOVER) != 0 &&
                !primary_surface_fire && (primary_water || secondary_water))
                return UnitOrder::none;
            if (!flies) {
                if (has_movement_object(world, actor, hooks))
                    return UnitOrder::attack_chase;
                if ((actor.flags & OA_UNIT_FLAG_BUILDING) != 0)
                    return UnitOrder::attack_nomove;
                return kamikaze();
            }
            if (dropped && !target_flies)
                return UnitOrder::air_strike;
            if (!dropped && target_flies)
                return UnitOrder::air_to_air;
            if (target_flies)
                return UnitOrder::none;
            if ((def->flags & OA_UNIT_DEF_FLAG_HOVER_ATTACK) != 0)
                return UnitOrder::air_to_ground_hover;
            return UnitOrder::air_to_ground;
        }
        case OrderCommand::blast:
            return (abilities & OA_UNIT_DEF_ABILITY_CAN_DGUN) != 0 ? UnitOrder::attack_special
                                                                   : UnitOrder::none;
        case OrderCommand::unload: {
            const bool loads = (abilities & OA_UNIT_DEF_ABILITY_CAN_LOAD) != 0;
            if (loads && flies && target != nullptr && target_airbase)
                return UnitOrder::vtol_landing;
            if (!loads)
                return UnitOrder::none;
            return by_air(UnitOrder::ground_unload, UnitOrder::vtol_unload);
        }
        case OrderCommand::load:
            if (target != nullptr && can_load_unit(world, actor, *target))
                return by_air(UnitOrder::ground_pickup, UnitOrder::vtol_pickup);
            return UnitOrder::none;
        case OrderCommand::guard:
            if ((abilities & OA_UNIT_DEF_ABILITY_CAN_GUARD) != 0 && allied)
                return by_air(UnitOrder::follow_ground, UnitOrder::vtol_follow);
            return UnitOrder::none;
        case OrderCommand::repair:
            return assistable() ? assist_order() : UnitOrder::none;
        case OrderCommand::patrol:
            if ((abilities & OA_UNIT_DEF_ABILITY_CAN_PATROL) == 0)
                return UnitOrder::none;
            if (!has_movement_object(world, actor, hooks))
                return UnitOrder::qpatrol;
            if ((abilities & OA_UNIT_DEF_ABILITY_CAN_REPAIR) == 0)
                return by_air(UnitOrder::patrol, UnitOrder::vtol_patrol);
            return by_air(UnitOrder::repair_patrol, UnitOrder::vtol_repair_patrol);
        case OrderCommand::stop:
            return UnitOrder::stop;
        case OrderCommand::teleport:
            return UnitOrder::teleport;
        case OrderCommand::reclaim:
            if ((abilities & OA_UNIT_DEF_ABILITY_CAN_RECLAMATE) == 0)
                return UnitOrder::none;
            if (position != nullptr) {
                if ((abilities & OA_UNIT_DEF_ABILITY_CAN_RESURRECT) != 0 &&
                    !hooks.rules.rules().orders.resurrector_reclaims_features.enabled &&
                    feature_here())
                    return UnitOrder::resurrect;
                if (feature_here())
                    return by_air(UnitOrder::reclaim, UnitOrder::vtol_reclaim);
            }
            if (target == nullptr)
                return UnitOrder::none;
            return by_air(UnitOrder::reclaim_unit, UnitOrder::vtol_reclaim_unit);
        case OrderCommand::capture:
            if ((abilities & OA_UNIT_DEF_ABILITY_CAN_CAPTURE) != 0 && target != nullptr &&
                actor.owner != target->owner)
                return UnitOrder::capture;
            return UnitOrder::none;
        case OrderCommand::build:
            if (def->build_ids != 0 && has_movement_object(world, actor, hooks))
                return by_air(UnitOrder::mobile_build, UnitOrder::vtol_mobile_build);
            return UnitOrder::none;
        default:
            return UnitOrder::none;
        }
    }
}

const char* unit_order_name(UnitOrder order) noexcept {
    switch (order) {
    case UnitOrder::none:
        return "";
    case UnitOrder::move_ground:
        return "MOVE_GROUND";
    case UnitOrder::vtol_move:
        return "VTOL_MOVE";
    case UnitOrder::qmove:
        return "QMOVE";
    case UnitOrder::attack_chase:
        return "ATTACK_CHASE";
    case UnitOrder::attack_nomove:
        return "ATTACK_NOMOVE";
    case UnitOrder::attack_kamikaze:
        return "ATTACK_KAMIKAZE";
    case UnitOrder::suppress:
        return "SUPPRESS";
    case UnitOrder::air_to_ground:
        return "AIRTOGROUND";
    case UnitOrder::air_to_ground_hover:
        return "AIRTOGROUNDHOVER";
    case UnitOrder::air_to_air:
        return "AIRTOAIR";
    case UnitOrder::air_strike:
        return "AIRSTRIKE";
    case UnitOrder::attack_special:
        return "ATTACKSPECIAL";
    case UnitOrder::ground_unload:
        return "GROUND_UNLOAD";
    case UnitOrder::vtol_unload:
        return "VTOL_UNLOAD";
    case UnitOrder::vtol_landing:
        return "VTOL_LANDING";
    case UnitOrder::ground_pickup:
        return "GROUND_PICKUP";
    case UnitOrder::vtol_pickup:
        return "VTOL_PICKUP";
    case UnitOrder::follow_ground:
        return "FOLLOW_GROUND";
    case UnitOrder::vtol_follow:
        return "VTOL_FOLLOW";
    case UnitOrder::repair_unit:
        return "REPAIRUNIT";
    case UnitOrder::vtol_repair_unit:
        return "VTOL_REPAIRUNIT";
    case UnitOrder::help_build:
        return "HELPBUILD";
    case UnitOrder::vtol_help_build:
        return "VTOL_HELPBUILD";
    case UnitOrder::patrol:
        return "PATROL";
    case UnitOrder::vtol_patrol:
        return "VTOL_PATROL";
    case UnitOrder::repair_patrol:
        return "REPAIRPATROL";
    case UnitOrder::vtol_repair_patrol:
        return "VTOL_REPAIRPATROL";
    case UnitOrder::qpatrol:
        return "QPATROL";
    case UnitOrder::stop:
        return "STOP";
    case UnitOrder::teleport:
        return "TELEPORT";
    case UnitOrder::reclaim:
        return "RECLAIM";
    case UnitOrder::vtol_reclaim:
        return "VTOL_RECLAIM";
    case UnitOrder::reclaim_unit:
        return "RECLAIMUNIT";
    case UnitOrder::vtol_reclaim_unit:
        return "VTOL_RECLAIMUNIT";
    case UnitOrder::resurrect:
        return "RESURRECT";
    case UnitOrder::capture:
        return "CAPTURE";
    case UnitOrder::mobile_build:
        return "MOBILEBUILD";
    case UnitOrder::vtol_mobile_build:
        return "VTOL_MOBILEBUILD";
    }
    return "";
}

ClickAction click_action(const World& world, OrderCommand command, OrderCursor cursor) noexcept {
    if (cursor == OrderCursor::select)
        return ClickAction::select_unit;
    if (static_cast<uint8_t>(cursor) > static_cast<uint8_t>(OrderCursor::build)) {
        const bool highlight_only = interface_type(world.game) == interface_right_click &&
                                    command == OrderCommand::default_order;
        return highlight_only ? ClickAction::clear_selection : ClickAction::none;
    }
    return ClickAction::issue_command;
}

void add_to_group_centre(GroupCentre& centre, const Unit& unit) noexcept {
    const auto add = [](int32_t sum, int32_t value) {
        return static_cast<int32_t>(
            static_cast<uint32_t>(sum) + static_cast<uint32_t>(int32_t{high_word(value)})
        );
    };
    centre.sum_x = add(centre.sum_x, unit.position.x);
    centre.sum_z = add(centre.sum_z, unit.position.z);
    ++centre.units;
}

GroupCentre selection_centre(const World& world, const Unit* target) noexcept {
    GroupCentre centre{};
    const Player* local = world_player(&world, world.game.local_player_index);
    if (local == nullptr)
        return centre;
    const Unit* first = world_unit(&world, local->first_unit);
    const Unit* last = world_unit(&world, local->last_unit);
    for (const Unit* unit = first; unit != nullptr && last != nullptr && unit <= last; ++unit)
        if ((unit->flags & OA_UNIT_FLAG_SELECTED) != 0 && unit != target)
            add_to_group_centre(centre, *unit);
    return centre;
}

bool order_keeps_group_shape(UnitOrder order) noexcept {
    switch (order) {
    case UnitOrder::move_ground:
    case UnitOrder::vtol_move:
    case UnitOrder::patrol:
    case UnitOrder::vtol_patrol:
    case UnitOrder::repair_patrol:
    case UnitOrder::vtol_repair_patrol:
        return true;
    default:
        return false;
    }
}

FixedVec3
group_order_point(const GroupCentre& centre, const Unit& actor, const FixedVec3& point) noexcept {
    if (centre.units == 0)
        return point;
    // The average whole pixel, truncated toward zero, as a 16.16 value.
    const auto units = static_cast<int32_t>(centre.units);
    const auto centre_x = static_cast<uint32_t>(centre.sum_x / units) << 16U;
    const auto centre_z = static_cast<uint32_t>(centre.sum_z / units) << 16U;
    const auto x = static_cast<uint32_t>(actor.position.x);
    const auto z = static_cast<uint32_t>(actor.position.z);
    // The whole part of a 16.16 displacement's square, in square map pixels.
    const auto square = [](uint32_t displacement) {
        const int64_t signed_displacement = static_cast<int32_t>(displacement);
        return static_cast<uint32_t>(
            static_cast<uint64_t>(signed_displacement * signed_displacement) >> 32U
        );
    };
    const auto distance = static_cast<int32_t>(square(z - centre_z) + square(x - centre_x));
    const auto spread = static_cast<int32_t>(centre.units * group_spread_per_unit);
    if (distance > spread)
        return point;
    return {
        static_cast<int32_t>(x + static_cast<uint32_t>(point.x) - centre_x),
        point.y,
        static_cast<int32_t>(static_cast<uint32_t>(point.z) + z - centre_z)
    };
}

uint32_t selection_orders(
    const World& world,
    OrderCommand command,
    const OrderCursorHooks& hooks,
    SelectionOrder* out,
    uint32_t capacity
) noexcept {
    const Unit* pointer_unit = nullptr;
    if (command_binds_cursor_unit(static_cast<uint8_t>(command)) && world.game.cursor_unit_id != 0)
        pointer_unit = world_unit_at(&world, world.game.cursor_unit_id);
    const FixedVec3 position = pointer_position(world.game);
    const Player* local = world_player(&world, world.game.local_player_index);
    if (local == nullptr)
        return 0;
    const GroupCentre centre = selection_centre(world, pointer_unit);
    const Unit* first = world_unit(&world, local->first_unit);
    const Unit* last = world_unit(&world, local->last_unit);
    uint32_t count = 0;
    for (const Unit* unit = first; unit != nullptr && last != nullptr && unit <= last; ++unit) {
        if ((unit->flags & OA_UNIT_FLAG_SELECTED) == 0 || unit == pointer_unit)
            continue;
        const UnitOrder order = unit_order(world, command, *unit, pointer_unit, &position, hooks);
        if (order == UnitOrder::none)
            continue;
        if (count == capacity)
            break;
        const FixedVec3 destination =
            order_keeps_group_shape(order) ? group_order_point(centre, *unit, position) : position;
        out[count++] = {unit, order, destination};
    }
    return count;
}

} // namespace oa::sim::gameplay_input
