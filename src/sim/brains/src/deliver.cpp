// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// One banked call at the seat's controller point. It delivers the queued
// events, then alliance and defeat changes, then onTick.

#include "internal.hpp"

#include "oa/sim/brains/handles.hpp"
#include "oa/sim/match_runtime.hpp"

#include "oa/core/feature_def.h"
#include "oa/core/world.h"
#include "oa/data/mission_types.hpp"
#include "oa/data/script_host/fixed.hpp"
#include "oa/sim/simulation_state.hpp"

#include "lua.hpp"

#include <cstddef>
#include <cstdint>

namespace oa::sim::brains {
namespace {

constexpr int seen_words = 1024;
constexpr uint8_t captured_kind = static_cast<uint8_t>(match_runtime::DeathKind::captured);
constexpr uint8_t cancelled_kind = static_cast<uint8_t>(match_runtime::DeathKind::cancelled);

void fill_seen(Seat& seat, uint64_t* bits) {
    for (int word = 0; word < seen_words; ++word)
        bits[word] = 0;
    if (seat.match == nullptr)
        return;
    try {
        const sim::detection::Sightings& sight = seat.match->sightings(seat.player);
        const uint16_t* slots = sight.seen;
        const uint32_t count = sight.seen_count;
        if (slots == nullptr)
            return;
        for (uint32_t index = 0; index < count; ++index) {
            const uint16_t slot = slots[index];
            bits[slot >> 6] |= uint64_t{1} << (slot & 63);
        }
    } catch (...) {
        for (int word = 0; word < seen_words; ++word)
            bits[word] = 0;
    }
}

bool slot_seen(const uint64_t* bits, uint16_t slot) {
    return (bits[slot >> 6] & (uint64_t{1} << (slot & 63))) != 0;
}

bool may_name(const Seat& seat, uint16_t slot, uint8_t owner, const uint64_t* seen) {
    if (slot == 0 || seat.match == nullptr)
        return false;
    if (seat.full_awareness || owner == seat.player)
        return true;
    if (seat.match->allied(seat.player, owner))
        return true;
    if (seen == nullptr)
        return false;
    return slot_seen(seen, slot);
}

bool death_delivered(const Seat& seat, const Event& event, const uint64_t* seen) {
    if (event.detail == captured_kind || seat.match == nullptr)
        return false;
    if (seat.full_awareness || event.from == seat.player)
        return true;
    if (seat.match->allied(seat.player, event.from))
        return true;
    return slot_seen(seen, event.unit_slot);
}

bool finished_cancelled(const Event* items, uint32_t count, uint32_t index) {
    const Event& finished = items[index];
    for (uint32_t later = index + 1; later < count; ++later) {
        const Event& candidate = items[later];
        if (candidate.kind == EventKind::died && candidate.detail == cancelled_kind &&
            candidate.unit_slot == finished.unit_slot &&
            candidate.unit_generation == finished.unit_generation)
            return true;
    }
    return false;
}

const Event* wreck_after(const Event* items, uint32_t count, uint32_t index) {
    const Event& death = items[index];
    for (uint32_t later = index + 1; later < count; ++later) {
        const Event& candidate = items[later];
        if (candidate.kind == EventKind::wreck && candidate.unit_slot == death.unit_slot &&
            candidate.unit_generation == death.unit_generation)
            return &candidate;
    }
    return nullptr;
}

const char* hook_name(EventKind kind) {
    switch (kind) {
    case EventKind::created:
        return "onUnitCreated";
    case EventKind::finished:
        return "onUnitFinished";
    case EventKind::damaged:
        return "onUnitDamaged";
    case EventKind::died:
        return "onUnitDestroyed";
    case EventKind::given:
        return "onUnitGiven";
    case EventKind::idle:
        return "onUnitIdle";
    case EventKind::order_ended:
        return "onCommandDone";
    case EventKind::carried:
        return "onUnitLoaded";
    case EventKind::dropped:
        return "onUnitUnloaded";
    case EventKind::seen:
        return "onUnitSeen";
    case EventKind::lost:
        return "onUnitLost";
    case EventKind::path_failed:
        return "onPathFailed";
    case EventKind::wreck:
        return nullptr;
    }
    return nullptr;
}

const char* damage_word(uint8_t kind) {
    if (kind == 1)
        return "weapon";
    if (kind == 2)
        return "paralyze";
    if (kind == 10)
        return "heal";
    return "environment";
}

bool start_hook(lua_State* state, int hooks, const char* name) {
    if (name == nullptr)
        return false;
    lua_getfield(state, hooks, name);
    if (lua_type(state, -1) != LUA_TFUNCTION) {
        lua_pop(state, 1);
        return false;
    }
    return true;
}

void push_ref(lua_State* state, Seat& seat, uint16_t slot, uint32_t generation) {
    UnitRef ref;
    ref.slot = slot;
    ref.generation = generation;
    push_unit(state, seat, ref);
}

void push_optional(lua_State* state, Seat& seat, uint16_t slot, uint32_t generation, bool present) {
    if (!present) {
        lua_pushnil(state);
        return;
    }
    push_ref(state, seat, slot, generation);
}

void push_wreck(lua_State* state, Seat& seat, const Event& wreck) {
    char name[128];
    std::size_t length = 0;
    {
        const oa::World& world = seat.match->state();
        if (world.feature_defs != nullptr && wreck.feature < world.feature_def_count) {
            const char* stored = world.feature_defs[wreck.feature].name;
            while (length < 127 && stored[length] != '\0') {
                name[length] = stored[length];
                ++length;
            }
        }
    }
    name[length] = '\0';
    lua_createtable(state, 0, 3);
    lua_pushlstring(state, name, length);
    lua_setfield(state, -2, "name");
    script_host::push_fixed(state, static_cast<int64_t>(wreck.x));
    lua_setfield(state, -2, "x");
    script_host::push_fixed(state, static_cast<int64_t>(wreck.z));
    lua_setfield(state, -2, "z");
}

void push_order_name(lua_State* state, uint8_t kind) {
    char text[64];
    std::size_t length = 0;
    {
        const auto names = oa::data::mission_types::registered_names();
        if (static_cast<std::size_t>(kind) < names.size()) {
            const char* source = names[kind].data();
            const std::size_t size = names[kind].size();
            length = size < 63 ? size : 63;
            for (std::size_t byte = 0; byte < length; ++byte)
                text[byte] = source[byte];
        }
    }
    text[length] = '\0';
    lua_pushlstring(state, text, length);
}

void call_event(
    lua_State* state,
    Seat& seat,
    int hooks,
    const Event& event,
    const Event* wreck,
    const uint64_t* seen
) {
    const char* name = hook_name(event.kind);
    if (!start_hook(state, hooks, name))
        return;
    switch (event.kind) {
    case EventKind::created:
    case EventKind::finished:
        push_ref(state, seat, event.unit_slot, event.unit_generation);
        push_optional(
            state,
            seat,
            event.other_slot,
            event.other_generation,
            event.other_slot != 0 && event.other_slot != event.unit_slot
        );
        lua_call(state, 2, 0);
        return;
    case EventKind::damaged:
        push_ref(state, seat, event.unit_slot, event.unit_generation);
        push_optional(
            state,
            seat,
            event.other_slot,
            event.other_generation,
            may_name(seat, event.other_slot, event.from, seen)
        );
        lua_pushinteger(state, static_cast<lua_Integer>(event.amount));
        lua_pushstring(state, damage_word(event.detail));
        lua_call(state, 4, 0);
        return;
    case EventKind::died:
        push_ref(state, seat, event.unit_slot, event.unit_generation);
        push_optional(
            state,
            seat,
            event.other_slot,
            event.other_generation,
            may_name(seat, event.other_slot, event.to, seen)
        );
        if (wreck == nullptr)
            lua_pushnil(state);
        else
            push_wreck(state, seat, *wreck);
        lua_call(state, 3, 0);
        return;
    case EventKind::given:
        push_ref(state, seat, event.unit_slot, event.unit_generation);
        push_optional(state, seat, event.other_slot, event.other_generation, event.other_slot != 0);
        lua_pushinteger(state, static_cast<lua_Integer>(event.from));
        lua_pushinteger(state, static_cast<lua_Integer>(event.to));
        lua_call(state, 4, 0);
        return;
    case EventKind::idle:
    case EventKind::seen:
    case EventKind::lost:
    case EventKind::path_failed:
        push_ref(state, seat, event.unit_slot, event.unit_generation);
        lua_call(state, 1, 0);
        return;
    case EventKind::order_ended:
        push_ref(state, seat, event.unit_slot, event.unit_generation);
        push_order_name(state, event.detail);
        lua_pushstring(state, event.to != 0 ? "done" : "failed");
        lua_call(state, 3, 0);
        return;
    case EventKind::carried:
    case EventKind::dropped:
        push_ref(state, seat, event.unit_slot, event.unit_generation);
        push_optional(state, seat, event.other_slot, event.other_generation, event.other_slot != 0);
        lua_call(state, 2, 0);
        return;
    case EventKind::wreck:
        lua_pop(state, 1);
        return;
    }
    lua_pop(state, 1);
}

void walk_events(lua_State* state, Seat& seat, int hooks, bool table, const uint64_t* seen) {
    const Event* items = seat.delivering.items;
    const uint32_t count = seat.delivering.count;
    for (uint32_t index = 0; index < count; ++index) {
        script_host::charge_work(state, script_host::element_charge);
        const Event& event = items[index];
        if (event.kind == EventKind::wreck)
            continue;
        if (event.kind == EventKind::finished && finished_cancelled(items, count, index))
            continue;
        if (event.kind == EventKind::died && !death_delivered(seat, event, seen))
            continue;
        if (!table)
            continue;
        const Event* wreck = nullptr;
        if (event.kind == EventKind::died)
            wreck = wreck_after(items, count, index);
        call_event(state, seat, hooks, event, wreck, seen);
    }
}

bool read_diplomacy(Seat& seat, bool* allied_now, bool* live_now) {
    try {
        const oa::World& world = seat.match->state();
        for (uint8_t player = 0; player < OA_PLAYER_COUNT; ++player) {
            allied_now[player] = seat.match->allied(seat.player, player);
            live_now[player] =
                sim::simulation_state::player_participating(world.game.players[player]);
        }
        return true;
    } catch (...) {
        return false;
    }
}

void report_diplomacy(lua_State* state, Seat& seat, int hooks, bool table) {
    bool allied_now[OA_PLAYER_COUNT];
    bool live_now[OA_PLAYER_COUNT];
    for (uint8_t player = 0; player < OA_PLAYER_COUNT; ++player) {
        allied_now[player] = false;
        live_now[player] = false;
    }
    if (seat.match == nullptr || !read_diplomacy(seat, allied_now, live_now))
        return;
    if (!seat.diplomacy_known) {
        for (uint8_t player = 0; player < OA_PLAYER_COUNT; ++player) {
            seat.allied_with[player] = allied_now[player];
            seat.participating[player] = live_now[player];
        }
        seat.diplomacy_known = true;
        return;
    }
    for (uint8_t other = 0; other < OA_PLAYER_COUNT; ++other) {
        if (other == seat.player || allied_now[other] == seat.allied_with[other])
            continue;
        const bool allied = allied_now[other];
        seat.allied_with[other] = allied;
        if (!table || !start_hook(state, hooks, "onAllianceChanged"))
            continue;
        lua_pushinteger(state, static_cast<lua_Integer>(other));
        lua_pushboolean(state, allied ? 1 : 0);
        lua_call(state, 2, 0);
    }
    for (uint8_t other = 0; other < OA_PLAYER_COUNT; ++other) {
        const bool was_playing = seat.participating[other];
        const bool playing = live_now[other];
        seat.participating[other] = playing;
        if (!was_playing || playing)
            continue;
        if (!table || !start_hook(state, hooks, "onPlayerDefeated"))
            continue;
        lua_pushinteger(state, static_cast<lua_Integer>(other));
        lua_call(state, 1, 0);
    }
}

void call_tick(lua_State* state, Seat& seat, int hooks) {
    if (!start_hook(state, hooks, "onTick"))
        return;
    const uint32_t tick = seat.match->state().game.tick;
    lua_pushinteger(state, static_cast<lua_Integer>(tick));
    lua_call(state, 1, 0);
}

extern "C" {
static int deliver_hooks(lua_State* state) {
    auto* seat = static_cast<Seat*>(lua_touserdata(state, 1));
    if (seat == nullptr || seat->match == nullptr)
        return 0;
    uint64_t seen_bits[seen_words];
    fill_seen(*seat, seen_bits);
    lua_rawgeti(state, LUA_REGISTRYINDEX, seat->hooks_ref);
    const int hooks = lua_gettop(state);
    const bool table = lua_istable(state, hooks) != 0;
    walk_events(state, *seat, hooks, table, seen_bits);
    report_diplomacy(state, *seat, hooks, table);
    if (table)
        call_tick(state, *seat, hooks);
    lua_pop(state, 1);
    return 0;
}
}

int push_tick(void* context, lua_State* state) {
    lua_pushcfunction(state, deliver_hooks);
    lua_pushlightuserdata(state, context);
    return 1;
}

} // namespace

bool deliver_turn(void* context, match_runtime::Match&, uint8_t) {
    auto* seat = static_cast<Seat*>(context);
    if (seat == nullptr || !seat->sandbox.has_value())
        return false;
    script_host::Sandbox& sandbox = *seat->sandbox;
    if (seat->stopped || sandbox.failed()) {
        seat->stopped = true;
        return true;
    }
    const EventBuffer held = seat->incoming;
    seat->incoming = seat->delivering;
    seat->delivering = held;
    seat->incoming.count = 0;
    sandbox.refill_bank();
    script_host::ScriptError error;
    script_host::CallHooks call;
    call.context = seat;
    call.push = push_tick;
    if (!sandbox.call_banked("tick", call, error))
        seat->stopped = true;
    return true;
}

} // namespace oa::sim::brains
