// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The read-only `me` object: the seat's player, economy, units, squads and
// the built-in player's knowledge snapshot. Nothing here walks the seat's
// units to count them, and nothing reads the match when the seat has none.

#include "handles_internal.hpp"

#include "internal.hpp"

#include "oa/base/game_math.hpp"
#include "oa/core/world.h"
#include "oa/data/limits.hpp"
#include "oa/data/script_host/f32.hpp"
#include "oa/data/script_host/fixed.hpp"
#include "oa/data/script_host/script_host.hpp"
#include "oa/sim/ai.hpp"
#include "oa/sim/match_runtime.hpp"

#include "lua.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>

namespace oa::sim::brains {
namespace {

char me_getter_token;
char me_method_token;
char me_values_token;

constexpr std::size_t most_members =
    static_cast<std::size_t>(data::limits::highest_units_per_player);
constexpr uint64_t geometry_charge = 4;

void push_nil(lua_State* lua) {
    lua_pushnil(lua);
}

void push_bool(lua_State* lua, bool value) {
    lua_pushboolean(lua, value ? 1 : 0);
}

void charge_one(lua_State* lua) {
    script_host::charge_work(lua, script_host::element_charge);
}

void charge_sequence(lua_State* lua, std::size_t elements) {
    script_host::charge_work(
        lua, script_host::element_charge * (static_cast<uint64_t>(elements) + 1)
    );
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

void ensure_table(lua_State* lua, const void* token) {
    lua_rawgetp(lua, LUA_REGISTRYINDEX, token);
    if (lua_type(lua, -1) == LUA_TTABLE)
        return;
    lua_pop(lua, 1);
    lua_newtable(lua);
    lua_pushvalue(lua, -1);
    lua_rawsetp(lua, LUA_REGISTRYINDEX, token);
}

const oa::Player* seat_record(lua_State* lua) {
    const Seat* seat = seat_of(lua);
    if (seat == nullptr || seat->player >= OA_PLAYER_COUNT)
        return nullptr;
    if (seat->match == nullptr) {
        if (seat->calls.host.world == nullptr)
            return nullptr;
        return &seat->calls.host.world->game.players[seat->player];
    }
    return &seat->match->state().game.players[seat->player];
}

void push_hidden(lua_State* lua, const char* name) {
    lua_rawgetp(lua, LUA_REGISTRYINDEX, &me_values_token);
    if (lua_type(lua, -1) != LUA_TTABLE) {
        lua_pop(lua, 1);
        push_nil(lua);
        return;
    }
    lua_getfield(lua, -1, name);
    lua_remove(lua, -2);
}

bool read_squad(lua_State* lua, int index, const char* method, uint32_t* squad_number) {
    if (lua_isinteger(lua, index) == 0) {
        luaL_error(lua, "%s expects a squad from 0 to 9", method);
        return false;
    }
    const lua_Integer value = lua_tointeger(lua, index);
    if (value < 0 || value > 9) {
        luaL_error(lua, "%s expects a squad from 0 to 9", method);
        return false;
    }
    *squad_number = static_cast<uint32_t>(value);
    return true;
}

bool kept_in_squad(const Seat& seat, uint32_t squad_number, uint16_t slot) noexcept {
    if (squad_number != 0)
        return true;
    oa::World* world = nullptr;
    if (seat.match == nullptr)
        world = seat.calls.host.world;
    else
        world = &seat.match->state();
    if (world == nullptr)
        return false;
    const oa::Unit* unit = oa::world_unit_at(world, slot);
    if (unit == nullptr || unit->type_index == 0 || !oa::unit_is_live_target(unit->flags))
        return false;
    if (seat.calls.unit_squad == nullptr)
        return unit->squad == 0;
    try {
        return seat.calls.unit_squad(const_cast<sim::ai::ComputerHost*>(&seat.calls.host), slot) ==
               0;
    } catch (const std::exception&) {
        return false;
    }
}

bool copy_members(
    const Seat& seat,
    uint32_t squad_number,
    bool filter_squad_zero,
    uint16_t* out,
    std::size_t room,
    std::size_t* used
) {
    *used = 0;
    if (seat.match == nullptr) {
        if (seat.calls.host.squad_size == nullptr || seat.calls.host.squad_member == nullptr)
            return true;
        try {
            const auto squad = static_cast<sim::ai::Squad>(squad_number);
            const uint32_t count =
                seat.calls.host.squad_size(seat.calls.host.context, seat.player, squad);
            for (uint32_t index = 0; index < count && *used < room; ++index) {
                const uint16_t member = seat.calls.host.squad_member(
                    seat.calls.host.context, seat.player, squad, index
                );
                if (filter_squad_zero && !kept_in_squad(seat, squad_number, member))
                    continue;
                out[*used] = member;
                *used += 1;
            }
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }
    try {
        if (seat.calls.host.squad_size != nullptr && seat.calls.host.squad_member != nullptr) {
            const auto squad = static_cast<sim::ai::Squad>(squad_number);
            const uint32_t count =
                seat.calls.host.squad_size(seat.calls.host.context, seat.player, squad);
            for (uint32_t index = 0; index < count && *used < room; ++index) {
                const uint16_t member = seat.calls.host.squad_member(
                    seat.calls.host.context, seat.player, squad, index
                );
                if (filter_squad_zero && !kept_in_squad(seat, squad_number, member))
                    continue;
                out[*used] = member;
                *used += 1;
            }
            return true;
        }
        const auto members = seat.match->squad_members(seat.player, squad_number);
        for (const uint16_t member : members) {
            if (*used >= room)
                break;
            if (filter_squad_zero && !kept_in_squad(seat, squad_number, member))
                continue;
            out[*used] = member;
            *used += 1;
        }
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

bool players_allied(const Seat& seat, uint8_t other) {
    if (seat.calls.host.allied != nullptr)
        return seat.calls.host.allied(seat.calls.host.context, seat.player, other);
    if (seat.match == nullptr)
        return false;
    return seat.match->allied(seat.player, other);
}

std::size_t occupied_count(const Seat& seat) noexcept {
    if (seat.player >= OA_PLAYER_COUNT)
        return 0;
    oa::World* world = nullptr;
    if (seat.match == nullptr)
        world = seat.calls.host.world;
    else
        world = &seat.match->state();
    if (world == nullptr)
        return 0;
    const oa::Player& record = world->game.players[seat.player];
    uint32_t span = 0;
    const oa::Unit* first = oa::world_player_units(world, &record, &span);
    if (first == nullptr)
        return 0;
    std::size_t found = 0;
    for (uint32_t step = 0; step < span; ++step) {
        if (first[step].type_index != 0)
            ++found;
    }
    return found;
}

void push_side(lua_State* lua, const Seat& seat, const oa::Player& record) {
    if (seat.match == nullptr) {
        push_nil(lua);
        return;
    }
    const oa::PlayerSetupInfo* info = oa::world_player_info(&seat.match->state(), &record);
    if (info == nullptr)
        push_nil(lua);
    else
        lua_pushinteger(lua, static_cast<lua_Integer>(info->side));
}

void push_team(lua_State* lua, const oa::Player& record) {
    if (record.team == OA_PLAYER_NO_TEAM)
        push_nil(lua);
    else
        lua_pushinteger(lua, static_cast<lua_Integer>(record.team));
}

int32_t wrapped_difference(int32_t later, int32_t earlier) noexcept {
    return static_cast<int32_t>(static_cast<uint32_t>(later) - static_cast<uint32_t>(earlier));
}

bool read_endpoint(
    lua_State* lua, int index, const char* function, oa::FixedVec3* out, bool* hidden
) {
    UnitRef ref{};
    if (to_unit(lua, index, &ref)) {
        const Seat* seat = seat_of(lua);
        if (seat == nullptr || unit_access(*seat, ref, Access::seen) != Read::allow) {
            *hidden = true;
            return true;
        }
        const oa::Unit* unit = oa::world_unit_at(&seat->match->state(), ref.slot);
        if (unit == nullptr) {
            *hidden = true;
            return true;
        }
        *out = unit->position;
        *hidden = false;
        return true;
    }
    *hidden = false;
    return read_position(lua, index, function, out);
}

extern "C" {

int me_index(lua_State* lua) {
    if (lua_type(lua, 2) != LUA_TSTRING)
        return 0;
    lua_rawgetp(lua, LUA_REGISTRYINDEX, &me_getter_token);
    if (lua_type(lua, -1) == LUA_TTABLE) {
        lua_pushvalue(lua, 2);
        lua_rawget(lua, -2);
        if (lua_isfunction(lua, -1) != 0) {
            lua_call(lua, 0, 1);
            return 1;
        }
        lua_pop(lua, 1);
    }
    lua_pop(lua, 1);
    lua_rawgetp(lua, LUA_REGISTRYINDEX, &me_method_token);
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

int me_newindex(lua_State* lua) {
    return luaL_error(lua, "me is read-only");
}

int getter_player(lua_State* lua) {
    charge_one(lua);
    push_hidden(lua, "player");
    return 1;
}

int getter_level(lua_State* lua) {
    charge_one(lua);
    push_hidden(lua, "level");
    return 1;
}

int getter_difficulty(lua_State* lua) {
    charge_one(lua);
    push_hidden(lua, "difficulty");
    return 1;
}

int getter_options(lua_State* lua) {
    charge_one(lua);
    push_hidden(lua, "options");
    return 1;
}

int getter_name(lua_State* lua) {
    charge_one(lua);
    const oa::Player* record = seat_record(lua);
    if (record == nullptr) {
        push_nil(lua);
        return 1;
    }
    push_text(lua, record->name, sizeof record->name);
    return 1;
}

int getter_side(lua_State* lua) {
    charge_one(lua);
    const Seat* seat = seat_of(lua);
    const oa::Player* record = seat_record(lua);
    if (seat == nullptr || record == nullptr) {
        push_nil(lua);
        return 1;
    }
    push_side(lua, *seat, *record);
    return 1;
}

int getter_side_name(lua_State* lua) {
    charge_one(lua);
    const Seat* seat = seat_of(lua);
    const oa::Player* record = seat_record(lua);
    if (seat == nullptr || seat->match == nullptr || record == nullptr) {
        push_nil(lua);
        return 1;
    }
    const oa::PlayerSetupInfo* info = oa::world_player_info(&seat->match->state(), record);
    if (info == nullptr || info->side >= 5) {
        push_nil(lua);
        return 1;
    }
    push_text(
        lua,
        seat->match->state().game.sides[info->side].name,
        sizeof seat->match->state().game.sides[info->side].name
    );
    return 1;
}

int getter_team(lua_State* lua) {
    charge_one(lua);
    const oa::Player* record = seat_record(lua);
    if (record == nullptr) {
        push_nil(lua);
        return 1;
    }
    push_team(lua, *record);
    return 1;
}

int getter_color(lua_State* lua) {
    charge_one(lua);
    const Seat* seat = seat_of(lua);
    const oa::Player* record = seat_record(lua);
    if (seat == nullptr || seat->match == nullptr || record == nullptr) {
        push_nil(lua);
        return 1;
    }
    const oa::PlayerSetupInfo* info = oa::world_player_info(&seat->match->state(), record);
    if (info == nullptr) {
        push_nil(lua);
        return 1;
    }
    lua_pushinteger(lua, static_cast<lua_Integer>(info->color));
    return 1;
}

int getter_energy(lua_State* lua) {
    charge_one(lua);
    const oa::Player* record = seat_record(lua);
    if (record == nullptr) {
        push_nil(lua);
        return 1;
    }
    script_host::push_f32(lua, record->energy);
    return 1;
}

int getter_metal(lua_State* lua) {
    charge_one(lua);
    const oa::Player* record = seat_record(lua);
    if (record == nullptr) {
        push_nil(lua);
        return 1;
    }
    script_host::push_f32(lua, record->metal);
    return 1;
}

int getter_energy_storage(lua_State* lua) {
    charge_one(lua);
    const oa::Player* record = seat_record(lua);
    if (record == nullptr) {
        push_nil(lua);
        return 1;
    }
    script_host::push_f32(lua, record->energy_storage);
    return 1;
}

int getter_metal_storage(lua_State* lua) {
    charge_one(lua);
    const oa::Player* record = seat_record(lua);
    if (record == nullptr) {
        push_nil(lua);
        return 1;
    }
    script_host::push_f32(lua, record->metal_storage);
    return 1;
}

int getter_energy_produced(lua_State* lua) {
    charge_one(lua);
    const oa::Player* record = seat_record(lua);
    if (record == nullptr) {
        push_nil(lua);
        return 1;
    }
    script_host::push_f32(lua, record->energy_produced);
    return 1;
}

int getter_metal_produced(lua_State* lua) {
    charge_one(lua);
    const oa::Player* record = seat_record(lua);
    if (record == nullptr) {
        push_nil(lua);
        return 1;
    }
    script_host::push_f32(lua, record->metal_produced);
    return 1;
}

int getter_energy_requested(lua_State* lua) {
    charge_one(lua);
    const oa::Player* record = seat_record(lua);
    if (record == nullptr) {
        push_nil(lua);
        return 1;
    }
    script_host::push_f32(lua, record->energy_requested);
    return 1;
}

int getter_metal_requested(lua_State* lua) {
    charge_one(lua);
    const oa::Player* record = seat_record(lua);
    if (record == nullptr) {
        push_nil(lua);
        return 1;
    }
    script_host::push_f32(lua, record->metal_requested);
    return 1;
}

int getter_energy_surplus(lua_State* lua) {
    charge_one(lua);
    const oa::Player* record = seat_record(lua);
    if (record == nullptr) {
        push_nil(lua);
        return 1;
    }
    lua_pushnumber(lua, oa::player_energy_surplus(record));
    return 1;
}

int getter_metal_surplus(lua_State* lua) {
    charge_one(lua);
    const oa::Player* record = seat_record(lua);
    if (record == nullptr) {
        push_nil(lua);
        return 1;
    }
    lua_pushnumber(lua, oa::player_metal_surplus(record));
    return 1;
}

int getter_tick(lua_State* lua) {
    charge_one(lua);
    const Seat* seat = seat_of(lua);
    if (seat == nullptr) {
        push_nil(lua);
        return 1;
    }
    if (seat->match == nullptr) {
        if (seat->calls.host.world == nullptr) {
            push_nil(lua);
            return 1;
        }
        lua_pushinteger(lua, static_cast<lua_Integer>(seat->calls.host.world->game.tick));
        return 1;
    }
    lua_pushinteger(lua, static_cast<lua_Integer>(seat->match->state().game.tick));
    return 1;
}

int getter_handed_over(lua_State* lua) {
    charge_one(lua);
    const Seat* seat = seat_of(lua);
    push_bool(lua, seat != nullptr && seat->handed_over);
    return 1;
}

int getter_refused(lua_State* lua) {
    charge_one(lua);
    const Seat* seat = seat_of(lua);
    if (seat == nullptr || seat->match == nullptr) {
        push_nil(lua);
        return 1;
    }
    lua_pushinteger(lua, static_cast<lua_Integer>(seat->refused_assignments));
    return 1;
}

int knowledge_owned(lua_State* lua) {
    charge_one(lua);
    if (lua_isinteger(lua, 2) == 0) {
        lua_pushinteger(lua, 0);
        return 1;
    }
    const lua_Integer type_index = lua_tointeger(lua, 2);
    if (type_index < 0) {
        lua_pushinteger(lua, 0);
        return 1;
    }
    lua_rawgeti(lua, lua_upvalueindex(1), type_index);
    if (lua_type(lua, -1) != LUA_TNUMBER) {
        lua_pop(lua, 1);
        lua_pushinteger(lua, 0);
    }
    return 1;
}

int getter_knowledge(lua_State* lua) {
    const Seat* seat = seat_of(lua);
    if (seat == nullptr || seat->match == nullptr) {
        charge_one(lua);
        push_nil(lua);
        return 1;
    }
    const auto& computer_slot = seat->match->computer_player_state();
    if (!computer_slot) {
        charge_one(lua);
        push_nil(lua);
        return 1;
    }
    auto* players = static_cast<sim::ai::ComputerPlayers*>(computer_slot.get());
    sim::ai::ComputerKnowledge* knowledge =
        sim::ai::computer_player_knowledge(players, seat->player);
    if (knowledge == nullptr) {
        charge_one(lua);
        push_nil(lua);
        return 1;
    }
    bool sight_ok = false;
    uint32_t refreshed = 0;
    try {
        refreshed = seat->match->sightings(seat->player).refreshed_tick;
        sight_ok = true;
    } catch (const std::exception&) {
        sight_ok = false;
    }
    if (!sight_ok) {
        charge_one(lua);
        push_nil(lua);
        return 1;
    }
    const int32_t builders = knowledge->builder_count;
    const oa::FixedVec3 base = knowledge->base_position;
    const uint32_t type_count = players->type_count;
    const int16_t* counts = knowledge->owned_counts;
    charge_sequence(lua, counts == nullptr ? 0 : type_count);
    lua_createtable(lua, 0, 4);
    lua_pushinteger(lua, static_cast<lua_Integer>(refreshed));
    lua_setfield(lua, -2, "tick");
    lua_pushinteger(lua, static_cast<lua_Integer>(builders));
    lua_setfield(lua, -2, "builders");
    push_position(lua, base);
    lua_setfield(lua, -2, "base");
    lua_newtable(lua);
    if (counts != nullptr) {
        for (uint32_t type_index = 0; type_index < type_count; ++type_index) {
            lua_pushinteger(lua, static_cast<lua_Integer>(counts[type_index]));
            lua_rawseti(lua, -2, static_cast<lua_Integer>(type_index));
        }
    }
    lua_pushcclosure(lua, knowledge_owned, 1);
    lua_setfield(lua, -2, "owned");
    return 1;
}

int method_allied(lua_State* lua) {
    if (lua_isinteger(lua, 2) == 0)
        return luaL_error(lua, "allied expects a player from 0 to 9");
    const lua_Integer which = lua_tointeger(lua, 2);
    if (which < 0 || which > 9)
        return luaL_error(lua, "allied expects a player from 0 to 9");
    charge_one(lua);
    const Seat* seat = seat_of(lua);
    if (seat == nullptr || seat->match == nullptr) {
        push_nil(lua);
        return 1;
    }
    bool allied = false;
    try {
        allied = players_allied(*seat, static_cast<uint8_t>(which));
    } catch (const std::exception&) {
        push_nil(lua);
        return 1;
    }
    push_bool(lua, allied);
    return 1;
}

int method_allies(lua_State* lua) {
    const Seat* seat = seat_of(lua);
    std::array<uint8_t, OA_PLAYER_COUNT> allies{};
    std::size_t found = 0;
    if (seat != nullptr && seat->match != nullptr) {
        for (uint8_t which = 0; which < OA_PLAYER_COUNT; ++which) {
            if (which == seat->player)
                continue;
            bool allied = false;
            try {
                allied = players_allied(*seat, which);
            } catch (const std::exception&) {
                allied = false;
            }
            if (!allied)
                continue;
            allies[found] = which;
            ++found;
        }
    }
    charge_sequence(lua, found);
    lua_createtable(lua, static_cast<int>(found), 0);
    for (std::size_t row = 0; row < found; ++row) {
        lua_pushinteger(lua, static_cast<lua_Integer>(allies[row]));
        lua_rawseti(lua, -2, static_cast<lua_Integer>(row + 1));
    }
    return 1;
}

int method_players(lua_State* lua) {
    const Seat* seat = seat_of(lua);
    std::array<uint8_t, OA_PLAYER_COUNT> listed{};
    std::size_t found = 0;
    if (seat != nullptr && seat->match != nullptr) {
        const oa::World& played = seat->match->state();
        for (uint8_t which = 0; which < OA_PLAYER_COUNT; ++which) {
            if (played.game.players[which].in_use == 0)
                continue;
            listed[found] = which;
            ++found;
        }
    }
    charge_sequence(lua, found);
    lua_createtable(lua, static_cast<int>(found), 0);
    const int sequence = lua_gettop(lua);
    if (seat == nullptr || seat->match == nullptr)
        return 1;
    const oa::World& world = seat->match->state();
    for (std::size_t row = 0; row < found; ++row) {
        const uint8_t which = listed[row];
        const oa::Player& record = world.game.players[which];
        lua_createtable(lua, 0, 7);
        lua_pushinteger(lua, static_cast<lua_Integer>(which));
        lua_setfield(lua, -2, "player");
        push_text(lua, record.name, sizeof record.name);
        lua_setfield(lua, -2, "name");
        push_side(lua, *seat, record);
        lua_setfield(lua, -2, "side");
        push_team(lua, record);
        lua_setfield(lua, -2, "team");
        bool allied = false;
        try {
            allied = players_allied(*seat, which);
        } catch (const std::exception&) {
            allied = false;
        }
        push_bool(lua, allied);
        lua_setfield(lua, -2, "allied");
        push_bool(lua, record.status == OA_PLAYER_STATUS_COMPUTER);
        lua_setfield(lua, -2, "computer");
        push_bool(lua, record.unit_count > 0);
        lua_setfield(lua, -2, "alive");
        lua_rawseti(lua, sequence, static_cast<lua_Integer>(row + 1));
    }
    return 1;
}

int method_units(lua_State* lua) {
    Seat* seat = seat_of(lua);
    const std::size_t found = seat == nullptr ? 0 : occupied_count(*seat);
    charge_sequence(lua, found);
    lua_createtable(lua, static_cast<int>(found), 0);
    if (seat == nullptr || found == 0)
        return 1;
    oa::World* world = nullptr;
    if (seat->match != nullptr)
        world = &seat->match->state();
    else
        world = seat->calls.host.world;
    if (world == nullptr || seat->player >= OA_PLAYER_COUNT)
        return 1;
    const oa::Player& record = world->game.players[seat->player];
    uint32_t span = 0;
    const oa::Unit* first = oa::world_player_units(world, &record, &span);
    if (first == nullptr)
        return 1;
    lua_Integer row = 0;
    const int sequence = lua_gettop(lua);
    for (uint32_t step = 0; step < span && static_cast<std::size_t>(row) < found; ++step) {
        if (first[step].type_index == 0)
            continue;
        push_unit(lua, *seat, seat_unit_ref(*seat, first[step].id));
        lua_rawseti(lua, sequence, ++row);
    }
    return 1;
}

int method_each_unit(lua_State* lua) {
    if (lua_type(lua, 2) != LUA_TFUNCTION)
        return luaL_error(lua, "each_unit expects a function");
    Seat* seat = seat_of(lua);
    std::array<UnitRef, most_members> held{};
    std::size_t found = 0;
    if (seat != nullptr && seat->player < OA_PLAYER_COUNT) {
        oa::World* world = nullptr;
        if (seat->match != nullptr)
            world = &seat->match->state();
        else
            world = seat->calls.host.world;
        if (world != nullptr) {
            const oa::Player& record = world->game.players[seat->player];
            uint32_t span = 0;
            const oa::Unit* first = oa::world_player_units(world, &record, &span);
            if (first != nullptr) {
                for (uint32_t step = 0; step < span && found < held.size(); ++step) {
                    if (first[step].type_index == 0)
                        continue;
                    held[found] = seat_unit_ref(*seat, first[step].id);
                    ++found;
                }
            }
        }
    }
    charge_sequence(lua, found);
    for (std::size_t row = 0; row < found; ++row) {
        lua_pushvalue(lua, 2);
        push_unit(lua, *seat, held[row]);
        lua_call(lua, 1, 0);
    }
    return 0;
}

int method_unit_count(lua_State* lua) {
    charge_one(lua);
    const Seat* seat = seat_of(lua);
    const std::size_t found = seat == nullptr ? 0 : occupied_count(*seat);
    lua_pushinteger(lua, static_cast<lua_Integer>(found));
    return 1;
}

int method_squad_size(lua_State* lua) {
    uint32_t squad_number = 0;
    if (!read_squad(lua, 2, "squad_size", &squad_number))
        return 0;
    charge_one(lua);
    const Seat* seat = seat_of(lua);
    std::size_t used = 0;
    bool failed = false;
    if (seat != nullptr && seat->match == nullptr) {
        if (seat->calls.host.squad_size != nullptr) {
            try {
                used = seat->calls.host.squad_size(
                    seat->calls.host.context,
                    seat->player,
                    static_cast<sim::ai::Squad>(squad_number)
                );
            } catch (const std::exception&) {
                failed = true;
            }
        }
    } else if (seat != nullptr && seat->match != nullptr) {
        try {
            if (seat->calls.host.squad_size != nullptr) {
                used = seat->calls.host.squad_size(
                    seat->calls.host.context,
                    seat->player,
                    static_cast<sim::ai::Squad>(squad_number)
                );
            } else {
                used = seat->match->squad_members(seat->player, squad_number).size();
            }
        } catch (const std::exception&) {
            failed = true;
        }
    }
    if (failed) {
        push_nil(lua);
        return 1;
    }
    lua_pushinteger(lua, static_cast<lua_Integer>(used));
    return 1;
}

int method_squad_member(lua_State* lua) {
    uint32_t squad_number = 0;
    if (!read_squad(lua, 2, "squad_member", &squad_number))
        return 0;
    if (lua_isinteger(lua, 3) == 0) {
        charge_one(lua);
        push_nil(lua);
        return 1;
    }
    const lua_Integer at = lua_tointeger(lua, 3);
    charge_one(lua);
    if (at < 1) {
        push_nil(lua);
        return 1;
    }
    Seat* seat = seat_of(lua);
    std::array<uint16_t, most_members> slots{};
    std::size_t used = 0;
    const bool copied = seat != nullptr &&
                        copy_members(*seat, squad_number, false, slots.data(), slots.size(), &used);
    if (!copied || static_cast<std::size_t>(at) > used) {
        push_nil(lua);
        return 1;
    }
    if (seat == nullptr) {
        push_nil(lua);
        return 1;
    }
    push_unit(lua, *seat, seat_unit_ref(*seat, slots[static_cast<std::size_t>(at - 1)]));
    return 1;
}

int method_squad(lua_State* lua) {
    uint32_t squad_number = 0;
    if (!read_squad(lua, 2, "squad", &squad_number))
        return 0;
    Seat* seat = seat_of(lua);
    std::array<uint16_t, most_members> slots{};
    std::size_t used = 0;
    const bool copied = seat != nullptr &&
                        copy_members(*seat, squad_number, true, slots.data(), slots.size(), &used);
    if (!copied) {
        charge_one(lua);
        push_nil(lua);
        return 1;
    }
    charge_sequence(lua, used);
    lua_createtable(lua, static_cast<int>(used), 0);
    if (seat == nullptr)
        return 1;
    const int sequence = lua_gettop(lua);
    for (std::size_t row = 0; row < used; ++row) {
        push_unit(lua, *seat, seat_unit_ref(*seat, slots[row]));
        lua_rawseti(lua, sequence, static_cast<lua_Integer>(row + 1));
    }
    return 1;
}

int method_assign(lua_State* lua) {
    UnitRef ref{};
    if (!to_unit(lua, 2, &ref))
        return luaL_error(lua, "assign expects a unit");
    uint32_t squad_number = 0;
    if (!read_squad(lua, 3, "assign", &squad_number))
        return 0;
    charge_one(lua);
    Seat* seat = seat_of(lua);
    if (seat == nullptr || !seat_owns(*seat, ref)) {
        if (seat != nullptr)
            seat->refused_assignments += 1;
        push_bool(lua, false);
        return 1;
    }
    bool failed = false;
    try {
        if (seat->calls.host.set_squad != nullptr) {
            seat->calls.host.set_squad(
                seat->calls.host.context, ref.slot, static_cast<sim::ai::Squad>(squad_number)
            );
        } else if (seat->match != nullptr) {
            seat->match->set_unit_squad(ref.slot, squad_number);
        } else {
            failed = true;
        }
    } catch (const std::exception&) {
        failed = true;
    }
    if (failed) {
        seat->refused_assignments += 1;
        push_bool(lua, false);
        return 1;
    }
    push_bool(lua, true);
    return 1;
}

int brain_distance(lua_State* lua) {
    oa::FixedVec3 from{};
    oa::FixedVec3 to{};
    bool from_hidden = false;
    bool to_hidden = false;
    if (!read_endpoint(lua, 1, "brain.distance", &from, &from_hidden))
        return 0;
    if (!read_endpoint(lua, 2, "brain.distance", &to, &to_hidden))
        return 0;
    script_host::charge_work(lua, geometry_charge);
    if (from_hidden || to_hidden) {
        push_nil(lua);
        return 1;
    }
    const int32_t dx = wrapped_difference(to.x, from.x);
    const int32_t dz = wrapped_difference(to.z, from.z);
    const auto length = oa::base::game_math::distance(dx, dz);
    script_host::push_fixed(lua, static_cast<int64_t>(length));
    return 1;
}

int brain_direction(lua_State* lua) {
    oa::FixedVec3 from{};
    oa::FixedVec3 to{};
    bool from_hidden = false;
    bool to_hidden = false;
    if (!read_endpoint(lua, 1, "brain.direction", &from, &from_hidden))
        return 0;
    if (!read_endpoint(lua, 2, "brain.direction", &to, &to_hidden))
        return 0;
    script_host::charge_work(lua, geometry_charge);
    if (from_hidden || to_hidden) {
        push_nil(lua);
        return 1;
    }
    const int32_t dx = wrapped_difference(to.x, from.x);
    const int32_t dz = wrapped_difference(to.z, from.z);
    const auto heading = oa::base::game_math::direction(dx, dz);
    lua_pushinteger(lua, static_cast<lua_Integer>(heading));
    return 1;
}

} // extern "C"

void move_field(lua_State* lua, int table_index, int hidden_index, const char* name) {
    lua_getfield(lua, table_index, name);
    lua_setfield(lua, hidden_index, name);
    lua_pushnil(lua);
    lua_setfield(lua, table_index, name);
}

void reject_duplicate(lua_State* lua, const char* name) {
    if (name_used(lua, &me_getter_token, name) || name_used(lua, &me_method_token, name))
        luaL_error(lua, "%s was added twice", name);
}

} // namespace

void add_me_getter(lua_State* lua, const char* name, lua_CFunction function) {
    if (name == nullptr || function == nullptr)
        return;
    ensure_table(lua, &me_getter_token);
    lua_pop(lua, 1);
    ensure_table(lua, &me_method_token);
    lua_pop(lua, 1);
    reject_duplicate(lua, name);
    ensure_table(lua, &me_getter_token);
    lua_pushcfunction(lua, function);
    lua_setfield(lua, -2, name);
    lua_pop(lua, 1);
}

void add_me_method(lua_State* lua, const char* name, lua_CFunction function) {
    if (name == nullptr || function == nullptr)
        return;
    ensure_table(lua, &me_getter_token);
    lua_pop(lua, 1);
    ensure_table(lua, &me_method_token);
    lua_pop(lua, 1);
    reject_duplicate(lua, name);
    ensure_table(lua, &me_method_token);
    lua_pushcfunction(lua, function);
    lua_setfield(lua, -2, name);
    lua_pop(lua, 1);
}

void set_hidden_difficulty(lua_State* lua, const char* difficulty) {
    if (lua == nullptr || difficulty == nullptr)
        return;
    lua_rawgetp(lua, LUA_REGISTRYINDEX, &me_values_token);
    if (lua_type(lua, -1) != LUA_TTABLE) {
        lua_pop(lua, 1);
        return;
    }
    lua_pushstring(lua, difficulty);
    lua_setfield(lua, -2, "difficulty");
    lua_pop(lua, 1);
}

void install_me(lua_State* lua, Seat& seat) {
    store_seat(lua, seat);
    lua_getglobal(lua, "me");
    if (lua_type(lua, -1) != LUA_TTABLE) {
        lua_pop(lua, 1);
        lua_newtable(lua);
        lua_pushvalue(lua, -1);
        lua_setglobal(lua, "me");
    }
    const int table_index = lua_gettop(lua);
    lua_newtable(lua);
    const int hidden_index = lua_gettop(lua);
    move_field(lua, table_index, hidden_index, "player");
    move_field(lua, table_index, hidden_index, "level");
    move_field(lua, table_index, hidden_index, "difficulty");
    move_field(lua, table_index, hidden_index, "options");
    lua_pushvalue(lua, hidden_index);
    lua_rawsetp(lua, LUA_REGISTRYINDEX, &me_values_token);

    lua_newtable(lua);
    lua_pushcfunction(lua, me_index);
    lua_setfield(lua, -2, "__index");
    lua_pushcfunction(lua, me_newindex);
    lua_setfield(lua, -2, "__newindex");
    lua_pushboolean(lua, 0);
    lua_setfield(lua, -2, "__metatable");
    lua_setmetatable(lua, table_index);
    lua_pop(lua, 2);

    add_me_getter(lua, "player", getter_player);
    add_me_getter(lua, "level", getter_level);
    add_me_getter(lua, "difficulty", getter_difficulty);
    add_me_getter(lua, "options", getter_options);
    add_me_getter(lua, "name", getter_name);
    add_me_getter(lua, "side", getter_side);
    add_me_getter(lua, "side_name", getter_side_name);
    add_me_getter(lua, "team", getter_team);
    add_me_getter(lua, "color", getter_color);
    add_me_getter(lua, "energy", getter_energy);
    add_me_getter(lua, "metal", getter_metal);
    add_me_getter(lua, "energy_storage", getter_energy_storage);
    add_me_getter(lua, "metal_storage", getter_metal_storage);
    add_me_getter(lua, "energy_produced", getter_energy_produced);
    add_me_getter(lua, "metal_produced", getter_metal_produced);
    add_me_getter(lua, "energy_requested", getter_energy_requested);
    add_me_getter(lua, "metal_requested", getter_metal_requested);
    add_me_getter(lua, "energy_surplus", getter_energy_surplus);
    add_me_getter(lua, "metal_surplus", getter_metal_surplus);
    add_me_getter(lua, "tick", getter_tick);
    add_me_getter(lua, "handed_over", getter_handed_over);
    add_me_getter(lua, "refused_assignments", getter_refused);
    add_me_getter(lua, "knowledge", getter_knowledge);
    add_me_method(lua, "allied", method_allied);
    add_me_method(lua, "allies", method_allies);
    add_me_method(lua, "players", method_players);
    add_me_method(lua, "units", method_units);
    add_me_method(lua, "each_unit", method_each_unit);
    add_me_method(lua, "unit_count", method_unit_count);
    add_me_method(lua, "squad_size", method_squad_size);
    add_me_method(lua, "squad_member", method_squad_member);
    add_me_method(lua, "squad", method_squad);
    add_me_method(lua, "assign", method_assign);

    lua_getglobal(lua, "brain");
    if (lua_type(lua, -1) != LUA_TTABLE) {
        lua_pop(lua, 1);
        lua_newtable(lua);
    }
    lua_pushcfunction(lua, brain_distance);
    lua_setfield(lua, -2, "distance");
    lua_pushcfunction(lua, brain_direction);
    lua_setfield(lua, -2, "direction");
    lua_setglobal(lua, "brain");
}

} // namespace oa::sim::brains
