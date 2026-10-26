// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/match_runtime/mission_unit_binding.hpp"

#include "oa/data/defs/unit_records.hpp"
#include "oa/data/mission_types.hpp"
#include "oa/sim/gameplay_input/order_cursor.hpp"
#include "oa/sim/simulation_state.hpp"
#include "oa/sim/scenario/state.hpp"

#include <cstdint>

namespace oa::sim::match_runtime {
namespace {

namespace mission_units = sim::mission_units;
namespace input = sim::gameplay_input;

MissionUnitBinding& binding_of(void* context) {
    return *static_cast<MissionUnitBinding*>(context);
}

void note_failure(MissionUnitBinding& binding, const char* message) {
    if (binding.failure.empty())
        binding.failure = message;
}

sim::ground_orders::Point point_of(const FixedVec3& position) noexcept {
    return {position.x, position.y, position.z};
}

bool movement_object(const Match& match, const Unit& unit) {
    return match.ground_runtime(unit.id) != nullptr;
}

input::OrderCursorHooks order_cursor_hooks(MissionUnitBinding& binding) {
    input::OrderCursorHooks hooks{};
    hooks.context = &binding;
    hooks.movement_object = [](void* context, const World&, const Unit& unit) {
        return movement_object(binding_of(context).match, unit);
    };
    hooks.rules = binding.match.rules_view();
    return hooks;
}

} // namespace

mission_units::Hooks mission_unit_hooks(MissionUnitBinding& binding) {
    mission_units::Hooks hooks{};
    hooks.context = &binding;
    hooks.find_def = [](void* context, const char* name) -> const UnitDef* {
        const auto& world = binding_of(context).match.state();
        return data::defs::unit_defs_find(world.unit_defs, world.unit_def_count, name);
    };
    hooks.type_id = [](void* context, const char* name) -> uint16_t {
        const auto& world = binding_of(context).match.state();
        return data::defs::unit_defs_type_id(world.unit_defs, world.unit_def_count, name);
    };
    hooks.player_active = [](void* context, int32_t player) {
        const auto& world = binding_of(context).match.state();
        const auto index = static_cast<uint8_t>(player);
        return index < OA_PLAYER_COUNT &&
               sim::simulation_state::player_slot_active(index, world.game.players[index]);
    };
    hooks.fatal = [](void* context, const char* message) {
        note_failure(binding_of(context), message);
    };
    // A structure's site snaps to its footprint grid and stands
    // at the height its yard would take there.
    hooks.snap_to_build_grid = [](void* context, const UnitDef& def, FixedVec3* position) {
        binding_of(context).match.snap_to_build_grid(def.type_id, *position);
    };
    hooks.create_unit = [](void* context,
                           uint8_t player,
                           uint16_t type,
                           const FixedVec3& position,
                           bool finished,
                           uint32_t state,
                           uint16_t requested_slot) -> Unit* {
        auto& binding = binding_of(context);
        sim::unit_spawn::Request request;
        request.player = player;
        request.type = type;
        request.position = {
            static_cast<uint32_t>(position.x),
            static_cast<uint32_t>(position.y),
            static_cast<uint32_t>(position.z)
        };
        request.finished = finished;
        request.state = state;
        request.requested_slot = requested_slot;
        auto* slot = binding.match.create(request);
        return slot != nullptr ? &slot->record : nullptr;
    };
    hooks.movement_object = [](void* context, const Unit& unit) {
        return movement_object(binding_of(context).match, unit);
    };
    // The command resolver's order, then the order-table index of its name.
    hooks.order_for = [](void* context,
                         mission_units::OrderCategory category,
                         Unit& unit,
                         Unit* target,
                         const FixedVec3* position) -> uint8_t {
        auto& binding = binding_of(context);
        const auto order = input::unit_order(
            binding.match.state(),
            static_cast<input::OrderCommand>(category),
            unit,
            target,
            position,
            order_cursor_hooks(binding)
        );
        return data::mission_types::index_for_name(input::unit_order_name(order));
    };
    hooks.order_named = [](void*, const char* name) {
        return data::mission_types::index_for_name(name);
    };
    hooks.queue_order = [](void* context,
                           uint8_t kind,
                           uint32_t flags,
                           Unit& unit,
                           Unit* target,
                           const FixedVec3* position,
                           int32_t param_a,
                           int32_t param_b) {
        auto& binding = binding_of(context);
        const auto point = position != nullptr ? point_of(*position) : sim::ground_orders::Point{};
        (void)binding.match.issue_order(
            unit.id,
            kind,
            flags != 0,
            target != nullptr ? target->id : uint16_t{0},
            position != nullptr ? &point : nullptr,
            param_a,
            param_b
        );
    };
    hooks.carry = [](void* context, Unit& child, Unit& parent, uint8_t piece, uint8_t mode) {
        auto& binding = binding_of(context);
        const auto link_piece = static_cast<int8_t>(piece);
        binding.match.set_carry_link(child.id, parent.id, link_piece, mode);
    };
    hooks.no_mission_units = [](void* context) {
        sim::scenario::disable(binding_of(context).match.scenario_controller());
    };
    // The match reaches the kill handler only through its death sweep: a live unit
    // is marked dying with the outcome and dies when the sweep next updates it.
    hooks.kill = [](void*, Unit& unit, uint8_t outcome) {
        if ((unit.flags & OA_UNIT_FLAG_LIVE) == 0)
            return;
        unit.damage_kind = outcome;
        unit.flags |= OA_UNIT_FLAG_DEATH_PENDING;
    };
    return hooks;
}

bool create_mission_units(Match& match, const data::campaign::MissionUnit* units, int32_t count) {
    MissionUnitBinding binding{match, {}};
    const auto hooks = mission_unit_hooks(binding);
    if (!mission_units::create_mission_units(match.state(), units, count, hooks)) {
        match.note_fault("mission unit table cannot be allocated");
        return false;
    }
    if (!binding.failure.empty()) {
        match.note_fault(binding.failure);
        return false;
    }
    return true;
}

} // namespace oa::sim::match_runtime
