// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The brain profile. install_libraries is the one place a later ticket adds
// a library: one call, after the ones already here, in the order the tickets
// name. It has to work when the seat has no match.

#include "internal.hpp"

#include "lua_game.hpp"

#include "oa/data/script_host/f32.hpp"
#include "oa/data/script_host/fixed.hpp"
#include "oa/data/script_host/geometry.hpp"
#include "oa/sim/unit_movement/movement.hpp"

#include "lua.hpp"

#include <cstdint>

namespace oa::sim::brains {
namespace {

constexpr int64_t component_min = int64_t{-2147483647} - 1;
constexpr int64_t component_max = 2147483647;
constexpr lua_Integer whole_min = -32768;
constexpr lua_Integer whole_max = 32767;

/// Reads the magnitude. An integer is whole world units, times 65536.
///
/// @return false after a raising call, so the caller returns as well
bool read_magnitude(lua_State* state, const char* function, int32_t* magnitude) {
    int64_t count = 0;
    if (script_host::to_fixed(state, 2, &count)) {
        if (count < component_min || count > component_max) {
            luaL_error(state, "geometry.%s: argument 2 is outside -32768 to 32767.99998", function);
            return false;
        }
        *magnitude = static_cast<int32_t>(count);
        return true;
    }
    if (lua_isinteger(state, 2) != 0) {
        const lua_Integer whole = lua_tointeger(state, 2);
        if (whole < whole_min || whole > whole_max) {
            luaL_error(state, "geometry.%s: argument 2 is outside -32768 to 32767.99998", function);
            return false;
        }
        const int64_t scaled = static_cast<int64_t>(whole) * int64_t{65536};
        *magnitude = static_cast<int32_t>(scaled);
        return true;
    }
    luaL_error(state, "geometry.%s: argument 2 must be a fixed value or an integer", function);
    return false;
}

template <typename Scaled>
int call_scaled(lua_State* state, const char* function, Scaled scaled) {
    if (lua_isinteger(state, 1) == 0) {
        luaL_error(state, "geometry.%s: argument 1 must be an integer", function);
        return 0;
    }
    const lua_Integer heading_bits = lua_tointeger(state, 1);
    const auto heading = static_cast<uint16_t>(heading_bits);
    int32_t magnitude = 0;
    if (!read_magnitude(state, function, &magnitude))
        return 0;
    const int32_t result = scaled(heading, magnitude);
    script_host::push_fixed(state, static_cast<int64_t>(result));
    return 1;
}

extern "C" {

static int geometry_sine_scaled(lua_State* state) {
    return call_scaled(state, "sine_scaled", sim::unit_movement::sine_scaled);
}

static int geometry_cosine_scaled(lua_State* state) {
    return call_scaled(state, "cosine_scaled", sim::unit_movement::cosine_scaled);
}

} // extern "C"

void add_scaled(lua_State* state) {
    lua_getglobal(state, "geometry");
    if (lua_type(state, -1) != LUA_TTABLE) {
        lua_pop(state, 1);
        lua_newtable(state);
    }
    lua_pushcfunction(state, geometry_sine_scaled);
    lua_setfield(state, -2, "sine_scaled");
    lua_pushcfunction(state, geometry_cosine_scaled);
    lua_setfield(state, -2, "cosine_scaled");
    lua_setglobal(state, "geometry");
}

void push_options(lua_State* state, const Seat& seat) {
    lua_createtable(state, 0, static_cast<int>(seat.option_count));
    for (uint8_t index = 0; index < seat.option_count; ++index) {
        const StoredOption& option = seat.options[index];
        if (option.kind == OptionKind::toggle)
            lua_pushboolean(state, option.toggle ? 1 : 0);
        else if (option.kind == OptionKind::number || option.text[0] == '\0')
            lua_pushnumber(state, option.number);
        else
            lua_pushstring(state, option.text);
        lua_setfield(state, -2, option.id);
    }
}

void push_me(lua_State* state, const Seat& seat) {
    lua_createtable(state, 0, 4);
    lua_pushinteger(state, static_cast<lua_Integer>(seat.player));
    lua_setfield(state, -2, "player");
    lua_pushstring(state, seat.level);
    lua_setfield(state, -2, "level");
    lua_pushstring(state, seat.difficulty);
    lua_setfield(state, -2, "difficulty");
    push_options(state, seat);
    lua_setfield(state, -2, "options");
    lua_setglobal(state, "me");
}

void install_profile(void* context, lua_State* state) {
    if (context == nullptr || state == nullptr)
        return;
    install_libraries(state, *static_cast<Seat*>(context));
}

} // namespace

void install_libraries(lua_State* state, Seat& seat) {
    script_host::open_f32_library(state);
    script_host::open_geometry_library(state);
    add_scaled(state);
    lua_newtable(state);
    lua_setglobal(state, "brain");
    lua_newtable(state);
    lua_setglobal(state, "game");
    push_me(state, seat);
    install_game(state, seat);
}

script_host::Profile brain_profile(Seat& seat, const script_host::Bank& bank) {
    script_host::Profile profile;
    profile.name = "brain";
    profile.context = &seat;
    profile.install = install_profile;
    profile.bank = bank;
    return profile;
}

} // namespace oa::sim::brains
