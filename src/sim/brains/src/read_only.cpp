// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Read-only views. The hidden table has no metatable, so a walk that calls
// the global `pairs` on it does not call back into this view.

#include "read_only.hpp"

#include "lua.hpp"

namespace oa::sim::brains {
namespace {

constexpr const char* fallback_denial = "game is read-only";

extern "C" {

static int deny_write(lua_State* state) {
    lua_pushvalue(state, lua_upvalueindex(1));
    return lua_error(state);
}

static int readonly_len(lua_State* state) {
    if (lua_getmetatable(state, 1) == 0) {
        lua_pushinteger(state, 0);
        return 1;
    }
    lua_getfield(state, -1, "__index");
    lua_remove(state, -2);
    lua_pushinteger(state, static_cast<lua_Integer>(lua_rawlen(state, -1)));
    return 1;
}

static int readonly_pairs(lua_State* state) {
    lua_getglobal(state, "pairs");
    if (lua_getmetatable(state, 1) == 0)
        return luaL_error(state, "%s", fallback_denial);
    lua_getfield(state, -1, "__index");
    lua_remove(state, -2);
    lua_call(state, 1, 3);
    return 3;
}

} // extern "C"

} // namespace

void make_read_only(lua_State* state, int proxy, int hidden, const char* denial) {
    proxy = lua_absindex(state, proxy);
    hidden = lua_absindex(state, hidden);
    if (denial == nullptr)
        denial = fallback_denial;
    lua_newtable(state);
    lua_pushvalue(state, hidden);
    lua_setfield(state, -2, "__index");
    lua_pushstring(state, denial);
    lua_pushcclosure(state, deny_write, 1);
    lua_setfield(state, -2, "__newindex");
    lua_pushcfunction(state, readonly_len);
    lua_setfield(state, -2, "__len");
    lua_pushcfunction(state, readonly_pairs);
    lua_setfield(state, -2, "__pairs");
    lua_pushboolean(state, 0);
    lua_setfield(state, -2, "__metatable");
    lua_setmetatable(state, proxy);
}

void push_read_only(lua_State* state, const char* denial) {
    lua_newtable(state);
    make_read_only(state, -1, -2, denial);
    lua_remove(state, -2);
}

} // namespace oa::sim::brains
