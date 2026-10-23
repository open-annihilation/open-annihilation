// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Heading and length for scripts. Each function calls the engine's own
// geometry. An integer argument is a whole number of world units. A fixed
// argument is a 16.16 count. The count has to fit in the 32-bit coordinate
// those functions take.

#include "oa/data/script_host/geometry.hpp"

#include "oa/data/script_host/fixed.hpp"

#include "lua.hpp"
#include "oa/base/game_math.hpp"

#include <cstdint>

namespace oa::data::script_host {
namespace {

constexpr int64_t component_min = int64_t{-2147483647} - 1;
constexpr int64_t component_max = 2147483647;
constexpr int64_t whole_min = -32768;
constexpr int64_t whole_max = 32767;

/// Reads one coordinate. An integer is whole world units, times 65536.
///
/// @return false after a raising call, so the caller returns as well
bool read_component(
    lua_State* state, int index, const char* function, int number, int32_t* component
) {
    int64_t count = 0;
    if (to_fixed(state, index, &count)) {
        if (count < component_min || count > component_max) {
            luaL_error(
                state, "geometry.%s: argument %d is outside -32768 to 32767.99998", function, number
            );
            return false;
        }
        *component = static_cast<int32_t>(count);
        return true;
    }
    if (lua_isinteger(state, index) != 0) {
        const int64_t whole = static_cast<int64_t>(lua_tointeger(state, index));
        if (whole < whole_min || whole > whole_max) {
            luaL_error(
                state, "geometry.%s: argument %d is outside -32768 to 32767.99998", function, number
            );
            return false;
        }
        *component = static_cast<int32_t>(whole * int64_t{65536});
        return true;
    }
    luaL_error(
        state, "geometry.%s: argument %d must be a fixed value or an integer", function, number
    );
    return false;
}

extern "C" {

static int geometry_direction(lua_State* state) {
    int32_t x = 0;
    int32_t z = 0;
    if (!read_component(state, 1, "direction", 1, &x))
        return 0;
    if (!read_component(state, 2, "direction", 2, &z))
        return 0;
    const uint16_t heading = oa::base::game_math::direction(x, z);
    lua_pushinteger(state, static_cast<lua_Integer>(heading));
    return 1;
}

static int geometry_planar_length(lua_State* state) {
    int32_t x = 0;
    int32_t z = 0;
    if (!read_component(state, 1, "planar_length", 1, &x))
        return 0;
    if (!read_component(state, 2, "planar_length", 2, &z))
        return 0;
    const uint32_t length = oa::base::game_math::distance(x, z);
    push_fixed(state, static_cast<int64_t>(length));
    return 1;
}

static int geometry_length(lua_State* state) {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    if (!read_component(state, 1, "length", 1, &x))
        return 0;
    if (!read_component(state, 2, "length", 2, &y))
        return 0;
    if (!read_component(state, 3, "length", 3, &z))
        return 0;
    const int32_t length = oa::base::game_math::truncated_length(x, y, z);
    push_fixed(state, static_cast<int64_t>(length));
    return 1;
}

static int geometry_scale(lua_State* state) {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    int32_t factor = 0;
    if (!read_component(state, 1, "scale", 1, &x))
        return 0;
    if (!read_component(state, 2, "scale", 2, &y))
        return 0;
    if (!read_component(state, 3, "scale", 3, &z))
        return 0;
    if (!read_component(state, 4, "scale", 4, &factor))
        return 0;
    const int32_t in[3] = {x, y, z};
    int32_t scaled[3] = {0, 0, 0};
    oa::base::game_math::scale_vector_fixed(scaled, in, factor);
    push_fixed(state, static_cast<int64_t>(scaled[0]));
    push_fixed(state, static_cast<int64_t>(scaled[1]));
    push_fixed(state, static_cast<int64_t>(scaled[2]));
    return 3;
}

} // extern "C"

} // namespace

void open_geometry_library(lua_State* state) {
    lua_newtable(state);
    lua_pushcfunction(state, geometry_direction);
    lua_setfield(state, -2, "direction");
    lua_pushcfunction(state, geometry_planar_length);
    lua_setfield(state, -2, "planar_length");
    lua_pushcfunction(state, geometry_length);
    lua_setfield(state, -2, "length");
    lua_pushcfunction(state, geometry_scale);
    lua_setfield(state, -2, "scale");
    lua_setglobal(state, "geometry");
}

} // namespace oa::data::script_host
