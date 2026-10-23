// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Unit handles and the fields a seat may read on its own units. Another
// player's unit stops at id and slot until a later ticket replaces
// seat_may_see.

#include "handles_internal.hpp"

#include "internal.hpp"

#include "oa/core/world.h"
#include "oa/data/mission_types.hpp"
#include "oa/data/script_host/f32.hpp"
#include "oa/data/script_host/fixed.hpp"
#include "oa/data/script_host/script_host.hpp"
#include "oa/sim/match_runtime.hpp"
#include "oa/sim/weapon_execution/retaliation.hpp"

#include "lua.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>

namespace oa::sim::brains {
namespace {

char seat_token;
char unit_getter_token;
char unit_method_token;
char unit_cache_token;
char unit_meta_token;

constexpr uint8_t order_preserve_busy = 0x08;
constexpr uint8_t order_queue_idle = 0x40;
constexpr std::size_t most_orders = 256;
constexpr int64_t component_min = int64_t{-2147483647} - 1;
constexpr int64_t component_max = 2147483647;
constexpr lua_Integer whole_min = -32768;
constexpr lua_Integer whole_max = 32767;

bool same_text(const char* text, const char* literal) noexcept {
    if (text == nullptr || literal == nullptr)
        return false;
    while (*text != '\0' && *text == *literal) {
        ++text;
        ++literal;
    }
    return *text == *literal;
}

void ensure_table(lua_State* lua, const void* token) {
    lua_rawgetp(lua, LUA_REGISTRYINDEX, token);
    if (lua_type(lua, -1) == LUA_TTABLE)
        return;
    lua_pop(lua, 1);
    lua_newtable(lua);
    lua_pushvalue(lua, -1);
    lua_rawsetp(lua, LUA_REGISTRYINDEX, token);
}

bool name_used(lua_State* lua, const void* token, const char* name) {
    lua_rawgetp(lua, LUA_REGISTRYINDEX, token);
    if (lua_type(lua, -1) != LUA_TTABLE) {
        lua_pop(lua, 1);
        return false;
    }
    lua_getfield(lua, -1, name);
    const bool used = lua_type(lua, -1) != LUA_TNIL;
    lua_pop(lua, 2);
    return used;
}

void reject_duplicate(lua_State* lua, const char* name) {
    if (name_used(lua, &unit_getter_token, name) || name_used(lua, &unit_method_token, name))
        luaL_error(lua, "%s was added twice", name);
}

const oa::Unit* arg_unit(lua_State* lua) {
    UnitRef ref{};
    if (!to_unit(lua, 1, &ref))
        return nullptr;
    const Seat* seat = seat_of(lua);
    if (seat == nullptr || seat->match == nullptr)
        return nullptr;
    return oa::world_unit_at(&seat->match->state(), ref.slot);
}

const oa::UnitDef* arg_def(lua_State* lua, const oa::Unit* unit) {
    const Seat* seat = seat_of(lua);
    if (seat == nullptr || seat->match == nullptr || unit == nullptr)
        return nullptr;
    return oa::world_unit_def_of(&seat->match->state(), unit);
}

void push_nil(lua_State* lua) {
    lua_pushnil(lua);
}

void push_bool(lua_State* lua, bool value) {
    lua_pushboolean(lua, value ? 1 : 0);
}

void push_kind(lua_State* lua, uint8_t kind) {
    const auto names = oa::data::mission_types::registered_names();
    if (kind >= names.size() || names[kind].empty()) {
        lua_pushliteral(lua, "");
        return;
    }
    lua_pushlstring(lua, names[kind].data(), names[kind].size());
}

const char* move_order_name(uint32_t flags) noexcept {
    const uint32_t order = (flags & OA_UNIT_FLAG_MOVE_ORDER_MASK) >> OA_UNIT_FLAG_MOVE_ORDER_SHIFT;
    if (order == 0)
        return "hold";
    if (order == 1)
        return "manoeuvre";
    if (order == 2)
        return "roam";
    return nullptr;
}

const char* fire_order_name(uint32_t flags) noexcept {
    const uint32_t order = (flags & OA_UNIT_FLAG_FIRE_ORDER_MASK) >> OA_UNIT_FLAG_FIRE_ORDER_SHIFT;
    if (order == 0)
        return "hold";
    if (order == 1)
        return "return";
    if (order == 2)
        return "at_will";
    return nullptr;
}

bool slot_in_range(const Seat& seat, uint16_t slot) noexcept {
    if (seat.match == nullptr || seat.player >= OA_PLAYER_COUNT)
        return false;
    const oa::Player& record = seat.match->state().game.players[seat.player];
    uint32_t span = 0;
    const oa::Unit* first = oa::world_player_units(&seat.match->state(), &record, &span);
    if (first == nullptr || span == 0)
        return false;
    const auto begin = static_cast<uint32_t>(first->id);
    return static_cast<uint32_t>(slot) >= begin && static_cast<uint32_t>(slot) < begin + span;
}

bool read_component(lua_State* lua, const char* function, const char* axis, int32_t* component) {
    int64_t count = 0;
    if (script_host::to_fixed(lua, -1, &count)) {
        if (count < component_min || count > component_max) {
            luaL_error(lua, "%s: %s must be a fixed value or a whole number", function, axis);
            return false;
        }
        *component = static_cast<int32_t>(count);
        return true;
    }
    if (lua_isinteger(lua, -1) != 0) {
        const lua_Integer whole = lua_tointeger(lua, -1);
        if (whole < whole_min || whole > whole_max) {
            luaL_error(lua, "%s: %s must be a fixed value or a whole number", function, axis);
            return false;
        }
        const int64_t scaled = static_cast<int64_t>(whole) * int64_t{65536};
        *component = static_cast<int32_t>(scaled);
        return true;
    }
    luaL_error(lua, "%s: %s must be a fixed value or a whole number", function, axis);
    return false;
}

extern "C" {

int unit_getter_thunk(lua_State* lua) {
    const auto access = static_cast<Access>(lua_tointeger(lua, lua_upvalueindex(1)));
    const auto* function =
        static_cast<const lua_CFunction*>(lua_touserdata(lua, lua_upvalueindex(2)));
    const bool alive_field = lua_tointeger(lua, lua_upvalueindex(3)) != 0;
    UnitRef ref{};
    if (function == nullptr || *function == nullptr || !to_unit(lua, 1, &ref)) {
        push_nil(lua);
        return 1;
    }
    const Seat* seat = seat_of(lua);
    if (seat == nullptr) {
        push_nil(lua);
        return 1;
    }
    script_host::charge_work(lua, script_host::element_charge);
    const Read decision = unit_access(*seat, ref, access);
    if (decision == Read::allow || (decision == Read::dead && alive_field))
        return (*function)(lua);
    push_nil(lua);
    return 1;
}

int unit_method_thunk(lua_State* lua) {
    const auto access = static_cast<Access>(lua_tointeger(lua, lua_upvalueindex(1)));
    const auto* function =
        static_cast<const lua_CFunction*>(lua_touserdata(lua, lua_upvalueindex(2)));
    const char* function_name = lua_tostring(lua, lua_upvalueindex(3));
    if (function_name == nullptr)
        function_name = "unit";
    UnitRef ref{};
    if (!to_unit(lua, 1, &ref))
        return luaL_error(lua, "%s expects a unit", function_name);
    const Seat* seat = seat_of(lua);
    const Read decision = seat == nullptr ? Read::none : unit_access(*seat, ref, access);
    if (decision != Read::allow || function == nullptr || *function == nullptr) {
        script_host::charge_work(lua, script_host::element_charge);
        push_nil(lua);
        return 1;
    }
    return (*function)(lua);
}

int unit_index(lua_State* lua) {
    if (lua_type(lua, 2) != LUA_TSTRING)
        return 0;
    lua_rawgetp(lua, LUA_REGISTRYINDEX, &unit_getter_token);
    if (lua_type(lua, -1) == LUA_TTABLE) {
        lua_pushvalue(lua, 2);
        lua_rawget(lua, -2);
        if (lua_isfunction(lua, -1) != 0) {
            lua_pushvalue(lua, 1);
            lua_call(lua, 1, 1);
            return 1;
        }
        lua_pop(lua, 1);
    }
    lua_pop(lua, 1);
    lua_rawgetp(lua, LUA_REGISTRYINDEX, &unit_method_token);
    if (lua_type(lua, -1) != LUA_TTABLE) {
        lua_pop(lua, 1);
        return 0;
    }
    lua_pushvalue(lua, 2);
    lua_rawget(lua, -2);
    if (lua_isfunction(lua, -1) != 0)
        return 1;
    return 0;
}

int unit_newindex(lua_State* lua) {
    return luaL_error(lua, "a unit handle cannot be assigned");
}

int unit_eq(lua_State* lua) {
    UnitRef left{};
    UnitRef right{};
    const bool same = to_unit(lua, 1, &left) && to_unit(lua, 2, &right) &&
                      left.slot == right.slot && left.generation == right.generation;
    push_bool(lua, same);
    return 1;
}

int unit_tostring(lua_State* lua) {
    UnitRef ref{};
    if (!to_unit(lua, 1, &ref))
        return luaL_error(lua, "a unit handle cannot be read");
    lua_pushfstring(
        lua, "unit %u.%u", static_cast<unsigned>(ref.slot), static_cast<unsigned>(ref.generation)
    );
    return 1;
}

int getter_id(lua_State* lua) {
    UnitRef ref{};
    if (!to_unit(lua, 1, &ref)) {
        push_nil(lua);
        return 1;
    }
    const int64_t id_value =
        static_cast<int64_t>(ref.generation) * int64_t{65536} + static_cast<int64_t>(ref.slot);
    lua_pushinteger(lua, static_cast<lua_Integer>(id_value));
    return 1;
}

int getter_slot(lua_State* lua) {
    UnitRef ref{};
    if (!to_unit(lua, 1, &ref)) {
        push_nil(lua);
        return 1;
    }
    lua_pushinteger(lua, static_cast<lua_Integer>(ref.slot));
    return 1;
}

int getter_alive(lua_State* lua) {
    UnitRef ref{};
    const Seat* seat = seat_of(lua);
    const bool current = to_unit(lua, 1, &ref) && seat != nullptr && seat->match != nullptr &&
                         unit_current(*seat->match, ref);
    push_bool(lua, current);
    return 1;
}

int getter_owner(lua_State* lua) {
    const oa::Unit* unit = arg_unit(lua);
    if (unit == nullptr) {
        push_nil(lua);
        return 1;
    }
    lua_pushinteger(lua, static_cast<lua_Integer>(unit->owner_index));
    return 1;
}

int getter_type_index(lua_State* lua) {
    const oa::Unit* unit = arg_unit(lua);
    if (unit == nullptr) {
        push_nil(lua);
        return 1;
    }
    lua_pushinteger(lua, static_cast<lua_Integer>(unit->type_index));
    return 1;
}

int getter_type_name(lua_State* lua) {
    const oa::Unit* unit = arg_unit(lua);
    const oa::UnitDef* def = arg_def(lua, unit);
    if (def == nullptr) {
        push_nil(lua);
        return 1;
    }
    push_text(lua, def->unit_name, sizeof def->unit_name);
    return 1;
}

int getter_x(lua_State* lua) {
    const oa::Unit* unit = arg_unit(lua);
    if (unit == nullptr) {
        push_nil(lua);
        return 1;
    }
    script_host::push_fixed(lua, unit->position.x);
    return 1;
}

int getter_y(lua_State* lua) {
    const oa::Unit* unit = arg_unit(lua);
    if (unit == nullptr) {
        push_nil(lua);
        return 1;
    }
    script_host::push_fixed(lua, unit->position.y);
    return 1;
}

int getter_z(lua_State* lua) {
    const oa::Unit* unit = arg_unit(lua);
    if (unit == nullptr) {
        push_nil(lua);
        return 1;
    }
    script_host::push_fixed(lua, unit->position.z);
    return 1;
}

int getter_position(lua_State* lua) {
    const oa::Unit* unit = arg_unit(lua);
    if (unit == nullptr) {
        push_nil(lua);
        return 1;
    }
    push_position(lua, unit->position);
    return 1;
}

int getter_heading(lua_State* lua) {
    const oa::Unit* unit = arg_unit(lua);
    if (unit == nullptr) {
        push_nil(lua);
        return 1;
    }
    lua_pushinteger(lua, static_cast<lua_Integer>(unit->heading));
    return 1;
}

int getter_health(lua_State* lua) {
    const oa::Unit* unit = arg_unit(lua);
    if (unit == nullptr) {
        push_nil(lua);
        return 1;
    }
    lua_pushinteger(lua, static_cast<lua_Integer>(unit->health));
    return 1;
}

int getter_health_percent(lua_State* lua) {
    const oa::Unit* unit = arg_unit(lua);
    if (unit == nullptr) {
        push_nil(lua);
        return 1;
    }
    lua_pushinteger(lua, static_cast<lua_Integer>(unit->health_percent));
    return 1;
}

int getter_max_health(lua_State* lua) {
    const oa::Unit* unit = arg_unit(lua);
    const oa::UnitDef* def = arg_def(lua, unit);
    if (def == nullptr) {
        push_nil(lua);
        return 1;
    }
    lua_pushinteger(lua, static_cast<lua_Integer>(def->max_damage));
    return 1;
}

int getter_build_remaining(lua_State* lua) {
    const oa::Unit* unit = arg_unit(lua);
    if (unit == nullptr) {
        push_nil(lua);
        return 1;
    }
    script_host::push_f32(lua, unit->build_remaining);
    return 1;
}

int getter_finished(lua_State* lua) {
    const oa::Unit* unit = arg_unit(lua);
    if (unit == nullptr) {
        push_nil(lua);
        return 1;
    }
    const float remaining = unit->build_remaining;
    push_bool(lua, !(remaining < 0.0F) && !(remaining > 0.0F));
    return 1;
}

int getter_building(lua_State* lua) {
    const oa::Unit* unit = arg_unit(lua);
    if (unit == nullptr) {
        push_nil(lua);
        return 1;
    }
    push_bool(lua, (unit->flags & OA_UNIT_FLAG_BUILDING) != 0);
    return 1;
}

int getter_armed(lua_State* lua) {
    const oa::Unit* unit = arg_unit(lua);
    if (unit == nullptr) {
        push_nil(lua);
        return 1;
    }
    push_bool(lua, (unit->flags & OA_UNIT_FLAG_HAS_WEAPONS) != 0);
    return 1;
}

int getter_selectable(lua_State* lua) {
    const oa::Unit* unit = arg_unit(lua);
    if (unit == nullptr) {
        push_nil(lua);
        return 1;
    }
    push_bool(lua, (unit->flags & OA_UNIT_FLAG_SELECTABLE) != 0);
    return 1;
}

int getter_live(lua_State* lua) {
    const oa::Unit* unit = arg_unit(lua);
    if (unit == nullptr) {
        push_nil(lua);
        return 1;
    }
    push_bool(lua, oa::unit_is_live_target(unit->flags));
    return 1;
}

int getter_dying(lua_State* lua) {
    const oa::Unit* unit = arg_unit(lua);
    if (unit == nullptr) {
        push_nil(lua);
        return 1;
    }
    push_bool(lua, (unit->flags & OA_UNIT_FLAG_DEATH_PENDING) != 0);
    return 1;
}

int getter_active(lua_State* lua) {
    const oa::Unit* unit = arg_unit(lua);
    if (unit == nullptr) {
        push_nil(lua);
        return 1;
    }
    push_bool(lua, (unit->state_flags & OA_UNIT_STATE_ACTIVE) != 0);
    return 1;
}

int getter_cloaked(lua_State* lua) {
    const oa::Unit* unit = arg_unit(lua);
    if (unit == nullptr) {
        push_nil(lua);
        return 1;
    }
    push_bool(lua, (unit->state_flags & OA_UNIT_STATE_CLOAKED) != 0);
    return 1;
}

int getter_mobile(lua_State* lua) {
    const oa::Unit* unit = arg_unit(lua);
    if (unit == nullptr) {
        push_nil(lua);
        return 1;
    }
    push_bool(lua, unit->movement != 0);
    return 1;
}

int getter_squad(lua_State* lua) {
    const oa::Unit* unit = arg_unit(lua);
    if (unit == nullptr) {
        push_nil(lua);
        return 1;
    }
    lua_pushinteger(lua, static_cast<lua_Integer>(unit->squad));
    return 1;
}

int getter_move_order(lua_State* lua) {
    const oa::Unit* unit = arg_unit(lua);
    if (unit == nullptr) {
        push_nil(lua);
        return 1;
    }
    const char* name = move_order_name(unit->flags);
    if (name == nullptr)
        push_nil(lua);
    else
        lua_pushstring(lua, name);
    return 1;
}

int getter_fire_order(lua_State* lua) {
    const oa::Unit* unit = arg_unit(lua);
    if (unit == nullptr) {
        push_nil(lua);
        return 1;
    }
    const char* name = fire_order_name(unit->flags);
    if (name == nullptr)
        push_nil(lua);
    else
        lua_pushstring(lua, name);
    return 1;
}

int getter_kills(lua_State* lua) {
    const oa::Unit* unit = arg_unit(lua);
    if (unit == nullptr) {
        push_nil(lua);
        return 1;
    }
    lua_pushinteger(lua, static_cast<lua_Integer>(unit->veteran_level));
    return 1;
}

int method_head_order(lua_State* lua) {
    UnitRef ref{};
    if (check_unit(lua, 1, "head_order", &ref) != 0)
        return 0;
    Seat* seat = seat_of(lua);

    struct HeadRead {
        bool present{};
        bool failed{};
        uint8_t kind{};
        bool busy{};
        bool idle{};
    } head{};

    if (seat == nullptr || seat->match == nullptr) {
        head.failed = true;
    } else {
        try {
            const match_runtime::Match& played = *seat->match;
            const auto* order = played.orders(ref.slot).primary;
            if (order != nullptr) {
                head.present = true;
                head.kind = order->kind;
                head.busy = (order->preserve_flags & order_preserve_busy) != 0;
                const uint8_t command = played.primary_order_command_flags(ref.slot);
                head.idle = (command & order_queue_idle) != 0;
            }
        } catch (const std::exception&) {
            head.failed = true;
        }
    }
    if (head.failed || !head.present) {
        push_nil(lua);
        return 1;
    }
    script_host::charge_work(lua, script_host::element_charge);
    push_kind(lua, head.kind);
    push_bool(lua, head.busy);
    push_bool(lua, head.idle);
    return 3;
}

int method_has_background_order(lua_State* lua) {
    UnitRef ref{};
    if (check_unit(lua, 1, "has_background_order", &ref) != 0)
        return 0;
    Seat* seat = seat_of(lua);
    bool failed = false;
    bool held = false;
    if (seat == nullptr || seat->match == nullptr) {
        failed = true;
    } else {
        try {
            const match_runtime::Match& played = *seat->match;
            held = played.orders(ref.slot).secondary != nullptr;
        } catch (const std::exception&) {
            failed = true;
        }
    }
    if (failed) {
        push_nil(lua);
        return 1;
    }
    script_host::charge_work(lua, script_host::element_charge);
    push_bool(lua, held);
    return 1;
}

int method_orders(lua_State* lua) {
    UnitRef ref{};
    if (check_unit(lua, 1, "orders", &ref) != 0)
        return 0;
    bool secondary = false;
    if (lua_gettop(lua) >= 2 && lua_type(lua, 2) != LUA_TNIL) {
        if (lua_type(lua, 2) != LUA_TBOOLEAN)
            return luaL_error(lua, "orders expects true or false");
        secondary = lua_toboolean(lua, 2) != 0;
    }
    Seat* seat = seat_of(lua);
    std::array<match_runtime::Match::OrderRecordView, most_orders> records{};
    std::size_t written = 0;
    bool failed = seat == nullptr || seat->match == nullptr;
    if (!failed) {
        try {
            written = seat->match->queue_records(ref.slot, secondary, records.data(), most_orders);
        } catch (const std::exception&) {
            failed = true;
        }
    }
    if (failed) {
        push_nil(lua);
        return 1;
    }
    script_host::charge_work(
        lua, script_host::element_charge * (static_cast<uint64_t>(written) + 1)
    );
    lua_createtable(lua, static_cast<int>(written), 0);
    for (std::size_t row = 0; row < written; ++row) {
        const match_runtime::Match::OrderRecordView& order_row = records[row];
        lua_createtable(lua, 0, 7);
        push_kind(lua, order_row.kind);
        lua_setfield(lua, -2, "kind");
        if (order_row.target == 0 || seat->match == nullptr)
            push_nil(lua);
        else
            push_unit(lua, *seat, unit_ref(*seat->match, order_row.target));
        lua_setfield(lua, -2, "target");
        script_host::push_fixed(lua, order_row.point[0]);
        lua_setfield(lua, -2, "x");
        script_host::push_fixed(lua, order_row.point[1]);
        lua_setfield(lua, -2, "y");
        script_host::push_fixed(lua, order_row.point[2]);
        lua_setfield(lua, -2, "z");
        lua_pushinteger(lua, static_cast<lua_Integer>(order_row.parameter_1));
        lua_setfield(lua, -2, "parameter_1");
        lua_pushinteger(lua, static_cast<lua_Integer>(order_row.parameter_2));
        lua_setfield(lua, -2, "parameter_2");
        lua_rawseti(lua, -2, static_cast<lua_Integer>(row + 1));
    }
    return 1;
}

int method_reaches(lua_State* lua) {
    UnitRef ref{};
    if (check_unit(lua, 1, "reaches", &ref) != 0)
        return 0;
    oa::FixedVec3 point{};
    if (!read_position(lua, 2, "reaches", &point))
        return 0;
    int weapon = 1;
    if (lua_gettop(lua) >= 3 && lua_type(lua, 3) != LUA_TNIL) {
        if (lua_isinteger(lua, 3) == 0)
            return luaL_error(lua, "reaches expects a weapon from 1 to 3");
        const lua_Integer which = lua_tointeger(lua, 3);
        if (which < 1 || which > 3)
            return luaL_error(lua, "reaches expects a weapon from 1 to 3");
        weapon = static_cast<int>(which);
    }
    script_host::charge_work(lua, 16);
    Seat* seat = seat_of(lua);
    bool failed = seat == nullptr || seat->match == nullptr;
    bool hits = false;
    if (!failed) {
        try {
            const oa::Unit* unit = oa::world_unit_at(&seat->match->state(), ref.slot);
            if (unit == nullptr)
                failed = true;
            else
                hits = sim::weapon_execution::slot_reaches_point(
                    seat->match->state(),
                    *unit,
                    point,
                    static_cast<uint8_t>(weapon - 1),
                    seat->match->rules_view()
                );
        } catch (const std::exception&) {
            failed = true;
        }
    }
    if (failed) {
        push_nil(lua);
        return 1;
    }
    push_bool(lua, hits);
    return 1;
}

} // extern "C"

void push_closure(
    lua_State* lua,
    lua_CFunction thunk,
    Access access,
    lua_CFunction function,
    int alive_field,
    const char* name
) {
    lua_pushinteger(lua, static_cast<lua_Integer>(static_cast<uint8_t>(access)));
    auto* held = static_cast<lua_CFunction*>(lua_newuserdatauv(lua, sizeof(lua_CFunction), 0));
    *held = function;
    if (name == nullptr)
        lua_pushinteger(lua, alive_field);
    else
        lua_pushstring(lua, name);
    lua_pushcclosure(lua, thunk, 3);
}

} // namespace

uint32_t unit_generation(match_runtime::Match& match, uint16_t slot) noexcept {
    if (static_cast<uint32_t>(slot) >= match.state().unit_slot_count)
        return 0;
    try {
        return match.runtime_state(slot).instance_generation;
    } catch (const std::exception&) {
        return 0;
    }
}

UnitRef unit_ref(match_runtime::Match& match, uint16_t slot) noexcept {
    return UnitRef{slot, unit_generation(match, slot)};
}

bool unit_current(match_runtime::Match& match, UnitRef ref) noexcept {
    if (static_cast<uint32_t>(ref.slot) >= match.state().unit_slot_count)
        return false;
    const oa::Unit* unit = oa::world_unit_at(&match.state(), ref.slot);
    if (unit == nullptr || unit->type_index == 0)
        return false;
    return unit_generation(match, ref.slot) == ref.generation;
}

bool seat_owns(const Seat& seat, UnitRef ref) noexcept {
    if (seat.match == nullptr || seat.player >= OA_PLAYER_COUNT)
        return false;
    if (!unit_current(*seat.match, ref))
        return false;
    const oa::Unit* unit = oa::world_unit_at(&seat.match->state(), ref.slot);
    return unit != nullptr && unit->owner_index == seat.player;
}

namespace {
bool see_none(const Seat&, UnitRef) noexcept {
    return false;
}
} // namespace

bool (*seat_may_see)(const Seat&, UnitRef) = see_none;

Read unit_access(const Seat& seat, UnitRef ref, Access access) noexcept {
    if (access == Access::always)
        return Read::allow;
    if (seat.match == nullptr)
        return Read::none;
    if (unit_current(*seat.match, ref)) {
        if (seat_owns(seat, ref))
            return Read::allow;
        if (access == Access::seen && seat_may_see != nullptr && seat_may_see(seat, ref))
            return Read::allow;
        return Read::none;
    }
    if (access == Access::seen && slot_in_range(seat, ref.slot))
        return Read::dead;
    return Read::none;
}

void store_seat(lua_State* lua, Seat& seat) {
    lua_pushlightuserdata(lua, &seat);
    lua_rawsetp(lua, LUA_REGISTRYINDEX, &seat_token);
}

Seat* seat_of(lua_State* lua) noexcept {
    if (lua == nullptr)
        return nullptr;
    if (lua_rawgetp(lua, LUA_REGISTRYINDEX, &seat_token) != LUA_TLIGHTUSERDATA) {
        lua_pop(lua, 1);
        return nullptr;
    }
    auto* seat = static_cast<Seat*>(lua_touserdata(lua, -1));
    lua_pop(lua, 1);
    return seat;
}

bool read_position(lua_State* lua, int index, const char* function, oa::FixedVec3* out) {
    if (out == nullptr) {
        luaL_error(lua, "%s expects a position: a table with x and z", function);
        return false;
    }
    const int absolute = lua_absindex(lua, index);
    if (lua_type(lua, absolute) != LUA_TTABLE) {
        luaL_error(lua, "%s expects a position: a table with x and z", function);
        return false;
    }
    lua_getfield(lua, absolute, "x");
    if (!read_component(lua, function, "x", &out->x))
        return false;
    lua_pop(lua, 1);
    lua_getfield(lua, absolute, "z");
    if (!read_component(lua, function, "z", &out->z))
        return false;
    lua_pop(lua, 1);
    lua_getfield(lua, absolute, "y");
    if (lua_type(lua, -1) == LUA_TNIL)
        out->y = 0;
    else if (!read_component(lua, function, "y", &out->y))
        return false;
    lua_pop(lua, 1);
    return true;
}

void push_position(lua_State* lua, const oa::FixedVec3& point) {
    lua_createtable(lua, 0, 3);
    script_host::push_fixed(lua, point.x);
    lua_setfield(lua, -2, "x");
    script_host::push_fixed(lua, point.y);
    lua_setfield(lua, -2, "y");
    script_host::push_fixed(lua, point.z);
    lua_setfield(lua, -2, "z");
}

int check_unit(lua_State* lua, int index, const char* function, UnitRef* ref) {
    if (ref == nullptr || !to_unit(lua, index, ref))
        return luaL_error(lua, "%s expects a unit", function);
    return 0;
}

void push_text(lua_State* lua, const char* text, std::size_t room) {
    std::size_t length = 0;
    if (text != nullptr) {
        while (length < room && text[length] != '\0')
            ++length;
    }
    lua_pushlstring(lua, text == nullptr ? "" : text, length);
}

void push_unit(lua_State* lua, Seat& seat, UnitRef ref) {
    store_seat(lua, seat);
    ensure_table(lua, &unit_cache_token);
    lua_rawgeti(lua, -1, static_cast<lua_Integer>(ref.slot));
    UnitRef cached{};
    if (to_unit(lua, -1, &cached) && cached.slot == ref.slot &&
        cached.generation == ref.generation) {
        lua_remove(lua, -2);
        return;
    }
    lua_pop(lua, 1);
    auto* memory = static_cast<UnitRef*>(lua_newuserdatauv(lua, sizeof(UnitRef), 0));
    *memory = ref;
    lua_rawgetp(lua, LUA_REGISTRYINDEX, &unit_meta_token);
    if (lua_type(lua, -1) == LUA_TTABLE)
        lua_setmetatable(lua, -2);
    else
        lua_pop(lua, 1);
    lua_pushvalue(lua, -1);
    lua_rawseti(lua, -3, static_cast<lua_Integer>(ref.slot));
    lua_remove(lua, -2);
}

bool to_unit(lua_State* lua, int index, UnitRef* ref) noexcept {
    if (lua == nullptr || ref == nullptr)
        return false;
    const int absolute = lua_absindex(lua, index);
    if (lua_type(lua, absolute) != LUA_TUSERDATA)
        return false;
    if (lua_getmetatable(lua, absolute) == 0)
        return false;
    lua_rawgetp(lua, LUA_REGISTRYINDEX, &unit_meta_token);
    const bool same = lua_rawequal(lua, -1, -2) != 0;
    lua_pop(lua, 2);
    if (!same)
        return false;
    const auto* memory = static_cast<const UnitRef*>(lua_touserdata(lua, absolute));
    if (memory == nullptr)
        return false;
    *ref = *memory;
    return true;
}

void add_unit_getter(lua_State* lua, const char* name, Access access, lua_CFunction function) {
    if (name == nullptr || function == nullptr)
        return;
    ensure_table(lua, &unit_getter_token);
    lua_pop(lua, 1);
    ensure_table(lua, &unit_method_token);
    lua_pop(lua, 1);
    reject_duplicate(lua, name);
    ensure_table(lua, &unit_getter_token);
    push_closure(
        lua, unit_getter_thunk, access, function, same_text(name, "alive") ? 1 : 0, nullptr
    );
    lua_setfield(lua, -2, name);
    lua_pop(lua, 1);
}

void add_unit_method(lua_State* lua, const char* name, Access access, lua_CFunction function) {
    if (name == nullptr || function == nullptr)
        return;
    ensure_table(lua, &unit_getter_token);
    lua_pop(lua, 1);
    ensure_table(lua, &unit_method_token);
    lua_pop(lua, 1);
    reject_duplicate(lua, name);
    ensure_table(lua, &unit_method_token);
    push_closure(lua, unit_method_thunk, access, function, 0, name);
    lua_setfield(lua, -2, name);
    lua_pop(lua, 1);
}

void install_handles(lua_State* lua, Seat& seat) {
    store_seat(lua, seat);
    ensure_table(lua, &unit_getter_token);
    lua_pop(lua, 1);
    ensure_table(lua, &unit_method_token);
    lua_pop(lua, 1);
    ensure_table(lua, &unit_cache_token);
    lua_pop(lua, 1);

    luaL_newmetatable(lua, "oa.brain.unit");
    lua_pushcfunction(lua, unit_index);
    lua_setfield(lua, -2, "__index");
    lua_pushcfunction(lua, unit_newindex);
    lua_setfield(lua, -2, "__newindex");
    lua_pushcfunction(lua, unit_eq);
    lua_setfield(lua, -2, "__eq");
    lua_pushcfunction(lua, unit_tostring);
    lua_setfield(lua, -2, "__tostring");
    lua_pushboolean(lua, 0);
    lua_setfield(lua, -2, "__metatable");
    lua_rawsetp(lua, LUA_REGISTRYINDEX, &unit_meta_token);

    add_unit_getter(lua, "id", Access::always, getter_id);
    add_unit_getter(lua, "slot", Access::always, getter_slot);
    add_unit_getter(lua, "alive", Access::seen, getter_alive);
    add_unit_getter(lua, "owner", Access::seen, getter_owner);
    add_unit_getter(lua, "type_index", Access::seen, getter_type_index);
    add_unit_getter(lua, "type_name", Access::seen, getter_type_name);
    add_unit_getter(lua, "x", Access::seen, getter_x);
    add_unit_getter(lua, "y", Access::seen, getter_y);
    add_unit_getter(lua, "z", Access::seen, getter_z);
    add_unit_getter(lua, "position", Access::seen, getter_position);
    add_unit_getter(lua, "heading", Access::seen, getter_heading);
    add_unit_getter(lua, "health", Access::seen, getter_health);
    add_unit_getter(lua, "health_percent", Access::seen, getter_health_percent);
    add_unit_getter(lua, "max_health", Access::seen, getter_max_health);
    add_unit_getter(lua, "build_remaining", Access::seen, getter_build_remaining);
    add_unit_getter(lua, "finished", Access::seen, getter_finished);
    add_unit_getter(lua, "building", Access::seen, getter_building);
    add_unit_getter(lua, "armed", Access::own, getter_armed);
    add_unit_getter(lua, "selectable", Access::own, getter_selectable);
    add_unit_getter(lua, "live", Access::own, getter_live);
    add_unit_getter(lua, "dying", Access::own, getter_dying);
    add_unit_getter(lua, "active", Access::own, getter_active);
    add_unit_getter(lua, "cloaked", Access::own, getter_cloaked);
    add_unit_getter(lua, "mobile", Access::own, getter_mobile);
    add_unit_getter(lua, "squad", Access::own, getter_squad);
    add_unit_getter(lua, "move_order", Access::own, getter_move_order);
    add_unit_getter(lua, "fire_order", Access::own, getter_fire_order);
    add_unit_getter(lua, "kills", Access::own, getter_kills);
    add_unit_method(lua, "head_order", Access::own, method_head_order);
    add_unit_method(lua, "has_background_order", Access::own, method_has_background_order);
    add_unit_method(lua, "orders", Access::own, method_orders);
    add_unit_method(lua, "reaches", Access::own, method_reaches);
}

} // namespace oa::sim::brains
