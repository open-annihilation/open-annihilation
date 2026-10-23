// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// brain.after, brain.every and brain.cancel. Due timers run in the seat's
// turn, after the events and the alliance and defeat changes and before
// onTick, ordered by due tick and then by sequence number.

#include "internal.hpp"

#include "oa/data/script_host/script_host.hpp"
#include "oa/sim/match_runtime.hpp"

#include "lua.hpp"

#include <cstdint>

static_assert(oa::sim::brains::timer_ref_empty == LUA_NOREF);

namespace oa::sim::brains {
namespace {

constexpr lua_Integer tick_limit = 2147483647;

const char* ticks_message = "ticks must be an integer from 1 to 2147483647";
const char* limit_message = "a brain may keep at most 1024 timers waiting";

int find_free(const Seat& seat) {
    for (int slot = 0; slot < static_cast<int>(max_timers); ++slot) {
        if (!seat.timers[slot].waiting)
            return slot;
    }
    return -1;
}

int find_waiting(const Seat& seat, uint32_t sequence) {
    for (int slot = 0; slot < static_cast<int>(max_timers); ++slot) {
        const WaitingTimer& timer = seat.timers[slot];
        if (timer.waiting && timer.sequence == sequence)
            return slot;
    }
    return -1;
}

bool release_timer(lua_State* state, WaitingTimer& timer) {
    if (!timer.waiting)
        return false;
    const int ref = timer.function_ref;
    timer.waiting = false;
    timer.function_ref = timer_ref_empty;
    timer.period = 0;
    if (ref != timer_ref_empty)
        luaL_unref(state, LUA_REGISTRYINDEX, ref);
    return true;
}

int create_timer(lua_State* state, const char* name, bool repeating) {
    if (lua_gettop(state) < 1 || lua_isinteger(state, 1) == 0)
        return luaL_error(state, "%s: %s", name, ticks_message);
    const lua_Integer ticks = lua_tointeger(state, 1);
    if (ticks < 1 || ticks > tick_limit)
        return luaL_error(state, "%s: %s", name, ticks_message);
    if (lua_gettop(state) != 2)
        return luaL_error(state, "%s: wrong number of arguments", name);
    if (lua_type(state, 2) != LUA_TFUNCTION)
        return luaL_error(state, "%s: argument 2 must be a function", name);
    auto* seat = static_cast<Seat*>(lua_touserdata(state, lua_upvalueindex(1)));
    if (seat == nullptr)
        return luaL_error(state, "%s: the seat is missing", name);
    const int slot = find_free(*seat);
    if (slot < 0)
        return luaL_error(state, "%s: %s", name, limit_message);
    lua_pushvalue(state, 2);
    const int ref = luaL_ref(state, LUA_REGISTRYINDEX);
    WaitingTimer& timer = seat->timers[slot];
    timer.function_ref = ref;
    timer.period = repeating ? static_cast<int32_t>(ticks) : 0;
    timer.sequence = seat->next_timer_sequence;
    ++seat->next_timer_sequence;
    const uint32_t now = seat->match != nullptr ? seat->match->state().game.tick : 0;
    timer.due = static_cast<int64_t>(now) + ticks;
    timer.waiting = true;
    lua_pushinteger(state, static_cast<lua_Integer>(timer.sequence));
    return 1;
}

extern "C" {

int brain_after(lua_State* state) {
    return create_timer(state, "brain.after", false);
}

int brain_every(lua_State* state) {
    return create_timer(state, "brain.every", true);
}

int brain_cancel(lua_State* state) {
    if (lua_gettop(state) != 1 || lua_isinteger(state, 1) == 0)
        return luaL_error(state, "brain.cancel: argument 1 must be an integer");
    const lua_Integer id = lua_tointeger(state, 1);
    auto* seat = static_cast<Seat*>(lua_touserdata(state, lua_upvalueindex(1)));
    bool removed = false;
    if (seat != nullptr && id > 0 && static_cast<uint64_t>(id) <= uint64_t{UINT32_MAX}) {
        const int slot = find_waiting(*seat, static_cast<uint32_t>(id));
        if (slot >= 0)
            removed = release_timer(state, seat->timers[slot]);
    }
    lua_pushboolean(state, removed ? 1 : 0);
    return 1;
}

} // extern "C"

bool timer_before(const WaitingTimer& left, const WaitingTimer& right) {
    if (left.due != right.due)
        return left.due < right.due;
    return left.sequence < right.sequence;
}

void bind_timer(lua_State* state, Seat& seat, const char* name, lua_CFunction function) {
    lua_getglobal(state, "brain");
    if (lua_type(state, -1) != LUA_TTABLE) {
        lua_pop(state, 1);
        return;
    }
    lua_pushlightuserdata(state, &seat);
    lua_pushcclosure(state, function, 1);
    lua_setfield(state, -2, name);
    lua_pop(state, 1);
}

} // namespace

void install_timers(lua_State* state, Seat& seat) {
    if (state == nullptr)
        return;
    bind_timer(state, seat, "after", brain_after);
    bind_timer(state, seat, "every", brain_every);
    bind_timer(state, seat, "cancel", brain_cancel);
}

void run_due_timers(lua_State* state, Seat& seat) {
    struct DueRef {
        uint16_t slot{};
        uint32_t sequence{};
    };

    if (state == nullptr || seat.match == nullptr)
        return;
    const int64_t tick = static_cast<int64_t>(seat.match->state().game.tick);
    DueRef due[max_timers];
    uint16_t due_count = 0;
    for (uint16_t slot = 0; slot < max_timers; ++slot) {
        const WaitingTimer& timer = seat.timers[slot];
        if (timer.waiting && timer.due <= tick) {
            due[due_count].slot = slot;
            due[due_count].sequence = timer.sequence;
            ++due_count;
        }
    }
    for (uint16_t sorted = 1; sorted < due_count; ++sorted) {
        const DueRef held = due[sorted];
        uint16_t place = sorted;
        while (place > 0 &&
               timer_before(seat.timers[held.slot], seat.timers[due[place - 1].slot])) {
            due[place] = due[place - 1];
            --place;
        }
        due[place] = held;
    }
    for (uint16_t due_at = 0; due_at < due_count; ++due_at) {
        WaitingTimer& timer = seat.timers[due[due_at].slot];
        // A callback may cancel this timer and reuse its slot. The sequence
        // number only grows, so a reused slot is a different timer.
        if (!timer.waiting || timer.sequence != due[due_at].sequence)
            continue;
        const int ref = timer.function_ref;
        const int32_t period = timer.period;
        if (period == 0 || timer.due > INT64_MAX - static_cast<int64_t>(period)) {
            timer.waiting = false;
            timer.function_ref = timer_ref_empty;
        } else {
            timer.due += period;
        }
        lua_rawgeti(state, LUA_REGISTRYINDEX, ref);
        if (!timer.waiting && ref != timer_ref_empty)
            luaL_unref(state, LUA_REGISTRYINDEX, ref);
        script_host::charge_work(state, script_host::element_charge);
        lua_call(state, 0, 0);
    }
}

} // namespace oa::sim::brains
