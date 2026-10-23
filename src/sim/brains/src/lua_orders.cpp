// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// One function gives every order. A refusal is counted and returned; it is
// never raised. The match call is the same one a click or the built-in
// player uses, and it runs in the next tick's unit sweep.
#include "orders.hpp"

#include "definitions.hpp"
#include "handles_internal.hpp"

#include "oa/sim/brains/classic.hpp"

#include "oa/core/unit.h"
#include "oa/core/world.h"
#include "oa/data/mission_types.hpp"
#include "oa/data/script_host/script_host.hpp"
#include "oa/sim/ai.hpp"
#include "oa/sim/gameplay_input/order_cursor.hpp"
#include "oa/sim/ground_orders/goals.hpp"
#include "oa/sim/match_runtime.hpp"
#include "oa/sim/match_runtime/attack_orders.hpp"

#include "lua.hpp"

#include <cstdint>
#include <cstring>
#include <span>

namespace oa::sim::brains {
namespace {

constexpr uint64_t order_charge = 64;

/// Why `give_order` stopped before the match, or that the actor may be ordered.
enum class ActorGate : uint8_t { ok, dead, not_own };

struct PointAsk {
    sim::ground_orders::Point point{};
    bool queued{};
};

struct TargetAsk {
    uint16_t target{};
    bool queued{};
};

struct BuildAsk {
    uint16_t type{};
    sim::ground_orders::Point point{};
    bool queued{};
    uint8_t facing{};
};

struct CountAsk {
    uint16_t type{};
    int32_t count{};
};

struct SpecialAsk {
    sim::ground_orders::Point point{};
    uint16_t target{};
    bool queued{};
    bool aimed{};
};

using Engine = bool (*)(match_runtime::Match& match, uint16_t slot, void* context);

/// What one order needs. `skip`, when it returns true, is not an order.
struct Give {
    UnitRef actor{};
    const UnitRef* target{};
    bool check_type{};
    uint16_t type{};
    bool (*skip)(match_runtime::Match& match, uint16_t slot, void* context){};
    Engine engine{};
    void* context{};
};

void count(Seat& seat, uint32_t OrderTally::* field) noexcept {
    ++(seat.turn_orders.*field);
    ++(seat.match_orders.*field);
}

ActorGate actor_gate(const Seat& seat, UnitRef actor) noexcept {
    if (seat.match == nullptr || !unit_current(*seat.match, actor))
        return ActorGate::dead;
    if (!seat_owns(seat, actor))
        return ActorGate::not_own;
    return ActorGate::ok;
}

bool refuse_actor(Seat& seat, UnitRef actor) noexcept {
    const ActorGate gate = actor_gate(seat, actor);
    if (gate == ActorGate::ok)
        return false;
    count(seat, gate == ActorGate::dead ? &OrderTally::dead : &OrderTally::not_own);
    return true;
}

/// A target the seat may name: its own live unit, a live unit under full
/// awareness, or one `seat_may_see` allows.
bool target_readable(const Seat& seat, UnitRef ref) noexcept {
    if (seat.match == nullptr || !unit_current(*seat.match, ref))
        return false;
    if (seat_owns(seat, ref))
        return true;
    if (seat.full_awareness)
        return true;
    return seat_may_see != nullptr && seat_may_see(seat, ref);
}

/// The builder's computer-player list, walked the way `game.build_list` walks it.
bool type_on_build_list(match_runtime::Match& match, uint16_t builder, uint16_t want) noexcept {
    sim::ai::ComputerPlayers* players = sim::ai::match_computer_players(match);
    if (players == nullptr || players->types == nullptr || players->build_list_runs == nullptr)
        return false;
    if (builder == 0 || builder >= players->type_count || want == 0 || want >= players->type_count)
        return false;
    if ((players->types[builder].flags & OA_UNIT_DEF_FLAG_BUILDER) == 0)
        return false;
    const uint16_t* run = players->build_list_runs;
    const uint16_t* const end = run + players->build_list_run_length;
    for (uint32_t type_index = 0; type_index < players->type_count && run < end; ++type_index) {
        const uint32_t listed = *run++;
        const auto present = static_cast<uint32_t>(end - run);
        const uint32_t length = listed < present ? listed : present;
        if (type_index == builder) {
            for (uint32_t entry = 0; entry < length; ++entry) {
                if (run[entry] == want)
                    return true;
            }
            return false;
        }
        run += length;
    }
    return false;
}

/// Gives one order. The first failing check wins: dead, not owned, the turn's
/// limit, the target, the type, then the match. A match refusal, a null, a
/// false or an exception is `by_engine`. This does not charge.
bool give_order(Seat& seat, const Give& give) {
    if (refuse_actor(seat, give.actor))
        return false;
    if (give.skip != nullptr && give.skip(*seat.match, give.actor.slot, give.context))
        return true;
    if (seat.orders_this_turn >= max_orders_per_turn) {
        count(seat, &OrderTally::over_limit);
        return false;
    }
    if (give.target != nullptr && !target_readable(seat, *give.target)) {
        count(seat, &OrderTally::not_seen);
        return false;
    }
    if (give.check_type) {
        const oa::Unit* unit = oa::world_unit_at(&seat.match->state(), give.actor.slot);
        const uint16_t builder = unit != nullptr ? unit->type_index : static_cast<uint16_t>(0);
        if (!type_on_build_list(*seat.match, builder, give.type)) {
            count(seat, &OrderTally::not_buildable);
            return false;
        }
    }
    ++seat.orders_this_turn;
    bool accepted = false;
    try {
        accepted =
            give.engine != nullptr && give.engine(*seat.match, give.actor.slot, give.context);
    } catch (...) {
        accepted = false;
    }
    count(seat, accepted ? &OrderTally::given : &OrderTally::by_engine);
    return accepted;
}

int finish(lua_State* state, bool given) {
    lua_pushboolean(state, given ? 1 : 0);
    return 1;
}

bool read_flag(lua_State* state, int index, const char* method, bool* queued) {
    if (lua_isnoneornil(state, index)) {
        *queued = false;
        return true;
    }
    if (lua_type(state, index) != LUA_TBOOLEAN) {
        luaL_error(state, "%s expects true or false", method);
        return false;
    }
    *queued = lua_toboolean(state, index) != 0;
    return true;
}

bool read_facing(lua_State* state, int index, const char* method, uint8_t* facing) {
    if (lua_isnoneornil(state, index)) {
        *facing = 0;
        return true;
    }
    if (lua_isinteger(state, index) == 0) {
        luaL_error(state, "%s expects a facing from 0 to 3", method);
        return false;
    }
    const lua_Integer value = lua_tointeger(state, index);
    if (value < 0 || value > 3) {
        luaL_error(state, "%s expects a facing from 0 to 3", method);
        return false;
    }
    *facing = static_cast<uint8_t>(value);
    return true;
}

bool read_required_bool(lua_State* state, int index, const char* method, bool* out) {
    if (lua_type(state, index) != LUA_TBOOLEAN) {
        luaL_error(state, "%s expects true or false", method);
        return false;
    }
    *out = lua_toboolean(state, index) != 0;
    return true;
}

Seat* charged_seat(lua_State* state, const char* method) {
    script_host::charge_work(state, order_charge);
    Seat* seat = seat_of(state);
    if (seat == nullptr)
        luaL_error(state, "%s has no seat", method);
    return seat;
}

sim::ground_orders::Point point_from(const oa::FixedVec3& at) noexcept {
    return {at.x, at.y, at.z};
}

bool engine_move(match_runtime::Match& match, uint16_t slot, void* context) {
    const auto* ask = static_cast<const PointAsk*>(context);
    return !match.order_refused(match.issue_ground_move(slot, ask->point, ask->queued));
}

bool engine_patrol(match_runtime::Match& match, uint16_t slot, void* context) {
    const auto* ask = static_cast<const PointAsk*>(context);
    return !match.order_refused(match.issue_patrol(slot, ask->point, ask->queued));
}

bool engine_attack(match_runtime::Match& match, uint16_t slot, void* context) {
    const auto* ask = static_cast<const TargetAsk*>(context);
    return match.issue_attack_command(slot, ask->target, ask->queued, nullptr);
}

bool engine_attack_ground(match_runtime::Match& match, uint16_t slot, void* context) {
    const auto* ask = static_cast<const PointAsk*>(context);
    return match.issue_attack_ground(slot, ask->point, ask->queued) != nullptr;
}

/// The built-in player's attack on a point: the Attack command over open
/// ground, unqueued. False when no order kind resolves.
bool engine_attack_point(match_runtime::Match& match, uint16_t slot, void* context) {
    const auto* at = static_cast<const oa::FixedVec3*>(context);
    const oa::World& world = match.state();
    const oa::Unit* actor = oa::world_unit_at(&world, slot);
    if (actor == nullptr)
        return false;
    const auto order = sim::gameplay_input::unit_order(
        world, sim::gameplay_input::OrderCommand::attack, *actor, nullptr, at, {}
    );
    const uint8_t kind =
        data::mission_types::index_for_name(sim::gameplay_input::unit_order_name(order));
    if (kind == data::mission_types::unknown_mission)
        return false;
    const sim::ground_orders::Point point = point_from(*at);
    return !match.order_refused(match.issue_order(slot, kind, false, 0, &point, 0, 0));
}

bool engine_dgun(match_runtime::Match& match, uint16_t slot, void* context) {
    const auto* ask = static_cast<const SpecialAsk*>(context);
    sim::ground_orders::Point point = ask->point;
    uint16_t target = 0;
    if (ask->aimed) {
        target = ask->target;
        const oa::Unit* aimed = oa::world_unit_at(&match.state(), target);
        if (aimed == nullptr)
            return false;
        point = point_from(aimed->position);
    }
    return !match.order_refused(match.issue_attack_special(slot, point, ask->queued, target));
}

bool engine_guard(match_runtime::Match& match, uint16_t slot, void* context) {
    const auto* ask = static_cast<const TargetAsk*>(context);
    return !match.order_refused(match.issue_guard(slot, ask->target, ask->queued));
}

bool engine_repair(match_runtime::Match& match, uint16_t slot, void* context) {
    const auto* ask = static_cast<const TargetAsk*>(context);
    return !match.order_refused(match.issue_repair(slot, ask->target, ask->queued));
}

bool engine_help(match_runtime::Match& match, uint16_t slot, void* context) {
    const auto* ask = static_cast<const TargetAsk*>(context);
    return !match.order_refused(match.issue_help_build(slot, ask->target, ask->queued));
}

bool engine_capture(match_runtime::Match& match, uint16_t slot, void* context) {
    const auto* ask = static_cast<const TargetAsk*>(context);
    return !match.order_refused(match.issue_capture(slot, ask->target, ask->queued));
}

bool engine_load(match_runtime::Match& match, uint16_t slot, void* context) {
    const auto* ask = static_cast<const TargetAsk*>(context);
    return !match.order_refused(match.issue_load(slot, ask->target, ask->queued));
}

bool engine_unload(match_runtime::Match& match, uint16_t slot, void* context) {
    const auto* ask = static_cast<const PointAsk*>(context);
    return !match.order_refused(match.issue_unload(slot, ask->point, ask->queued));
}

bool engine_reclaim_unit(match_runtime::Match& match, uint16_t slot, void* context) {
    const auto* ask = static_cast<const TargetAsk*>(context);
    return !match.order_refused(match.issue_reclaim(slot, ask->target, ask->queued));
}

bool engine_reclaim_point(match_runtime::Match& match, uint16_t slot, void* context) {
    const auto* ask = static_cast<const PointAsk*>(context);
    return !match.order_refused(match.issue_feature_reclaim(slot, ask->point, ask->queued));
}

bool engine_build(match_runtime::Match& match, uint16_t slot, void* context) {
    const auto* ask = static_cast<const BuildAsk*>(context);
    return !match.order_refused(
        match.issue_mobile_build(slot, ask->type, ask->point, ask->queued, ask->facing)
    );
}

bool engine_queue(match_runtime::Match& match, uint16_t slot, void* context) {
    const auto* ask = static_cast<const CountAsk*>(context);
    match.queue_factory_build(slot, ask->type, ask->count);
    return true;
}

bool engine_requeue(match_runtime::Match& match, uint16_t slot, void* context) {
    const auto* ask = static_cast<const CountAsk*>(context);
    const uint8_t kind = data::mission_types::index_for_name("BuildingBuild");
    if (kind == data::mission_types::unknown_mission)
        return false;
    match.change_queued_count(slot, kind, ask->type, ask->count);
    return true;
}

bool engine_stop(match_runtime::Match& match, uint16_t slot, void*) {
    return !match.order_refused(match.issue_stop(slot));
}

bool engine_on(match_runtime::Match& match, uint16_t slot, void*) {
    match.toggle_activation(slot, OA_UNIT_STATE_ACTIVE);
    return true;
}

bool engine_cloak(match_runtime::Match& match, uint16_t slot, void* context) {
    const auto* on = static_cast<const bool*>(context);
    return !match.order_refused(match.issue_cloak(slot, *on));
}

bool engine_destruct(match_runtime::Match& match, uint16_t slot, void*) {
    const uint16_t unit = slot;
    match.toggle_self_destruct(std::span<const uint16_t>(&unit, 1));
    return true;
}

bool engine_state(match_runtime::Match& match, uint16_t slot, void* context) {
    const auto* ask = static_cast<const CountAsk*>(context);
    return !match.order_refused(
        match.issue_state_order(slot, static_cast<uint8_t>(ask->type), ask->count)
    );
}

/// True when the active bit is already the value `on` asks for, so `on` is not an order.
bool activation_already(match_runtime::Match& match, uint16_t slot, void* context) {
    const auto* want = static_cast<const bool*>(context);
    const oa::Unit* unit = oa::world_unit_at(&match.state(), slot);
    if (unit == nullptr)
        return false;
    const bool active = (unit->state_flags & OA_UNIT_STATE_ACTIVE) != 0;
    return active == *want;
}

int give_point(lua_State* state, const char* method, Engine engine) {
    UnitRef actor{};
    if (check_unit(state, 1, method, &actor) != 0)
        return 0;
    oa::FixedVec3 at{};
    if (!read_position(state, 2, method, &at))
        return 0;
    bool queued = false;
    if (!read_flag(state, 3, method, &queued))
        return 0;
    Seat* seat = charged_seat(state, method);
    PointAsk ask{point_from(at), queued};
    Give give;
    give.actor = actor;
    give.engine = engine;
    give.context = &ask;
    return finish(state, give_order(*seat, give));
}

int give_target(lua_State* state, const char* method, Engine engine) {
    UnitRef actor{};
    if (check_unit(state, 1, method, &actor) != 0)
        return 0;
    UnitRef target{};
    if (check_unit(state, 2, method, &target) != 0)
        return 0;
    bool queued = false;
    if (!read_flag(state, 3, method, &queued))
        return 0;
    Seat* seat = charged_seat(state, method);
    TargetAsk ask{target.slot, queued};
    Give give;
    give.actor = actor;
    give.target = &target;
    give.engine = engine;
    give.context = &ask;
    return finish(state, give_order(*seat, give));
}

extern "C" {

int method_move(lua_State* state) {
    return give_point(state, "move", engine_move);
}

int method_patrol(lua_State* state) {
    return give_point(state, "patrol", engine_patrol);
}

int method_attack(lua_State* state) {
    return give_target(state, "attack", engine_attack);
}

int method_attack_ground(lua_State* state) {
    return give_point(state, "attack_ground", engine_attack_ground);
}

int method_attack_point(lua_State* state) {
    UnitRef actor{};
    if (check_unit(state, 1, "attack_point", &actor) != 0)
        return 0;
    oa::FixedVec3 at{};
    if (!read_position(state, 2, "attack_point", &at))
        return 0;
    Seat* seat = charged_seat(state, "attack_point");
    Give give;
    give.actor = actor;
    give.engine = engine_attack_point;
    give.context = &at;
    return finish(state, give_order(*seat, give));
}

int method_dgun(lua_State* state) {
    UnitRef actor{};
    if (check_unit(state, 1, "dgun", &actor) != 0)
        return 0;
    const int kind = lua_type(state, 2);
    UnitRef target{};
    oa::FixedVec3 at{};
    SpecialAsk ask{};
    if (kind == LUA_TTABLE) {
        if (!read_position(state, 2, "dgun", &at))
            return 0;
        ask.point = point_from(at);
    } else if (kind == LUA_TUSERDATA) {
        if (check_unit(state, 2, "dgun", &target) != 0)
            return 0;
        ask.aimed = true;
        ask.target = target.slot;
    } else {
        return luaL_error(state, "dgun expects a position or a unit");
    }
    if (!read_flag(state, 3, "dgun", &ask.queued))
        return 0;
    Seat* seat = charged_seat(state, "dgun");
    Give give;
    give.actor = actor;
    if (ask.aimed)
        give.target = &target;
    give.engine = engine_dgun;
    give.context = &ask;
    return finish(state, give_order(*seat, give));
}

int method_guard(lua_State* state) {
    return give_target(state, "guard", engine_guard);
}

int method_repair(lua_State* state) {
    return give_target(state, "repair", engine_repair);
}

int method_help(lua_State* state) {
    return give_target(state, "help", engine_help);
}

int method_capture(lua_State* state) {
    return give_target(state, "capture", engine_capture);
}

int method_load(lua_State* state) {
    return give_target(state, "load", engine_load);
}

int method_unload(lua_State* state) {
    return give_point(state, "unload", engine_unload);
}

int method_reclaim(lua_State* state) {
    UnitRef actor{};
    if (check_unit(state, 1, "reclaim", &actor) != 0)
        return 0;
    const int kind = lua_type(state, 2);
    if (kind == LUA_TTABLE) {
        oa::FixedVec3 at{};
        if (!read_position(state, 2, "reclaim", &at))
            return 0;
        bool queued = false;
        if (!read_flag(state, 3, "reclaim", &queued))
            return 0;
        Seat* seat = charged_seat(state, "reclaim");
        PointAsk ask{point_from(at), queued};
        Give give;
        give.actor = actor;
        give.engine = engine_reclaim_point;
        give.context = &ask;
        return finish(state, give_order(*seat, give));
    }
    if (kind != LUA_TUSERDATA)
        return luaL_error(state, "reclaim expects a unit or a position");
    UnitRef target{};
    if (check_unit(state, 2, "reclaim", &target) != 0)
        return 0;
    bool queued = false;
    if (!read_flag(state, 3, "reclaim", &queued))
        return 0;
    Seat* seat = charged_seat(state, "reclaim");
    TargetAsk ask{target.slot, queued};
    Give give;
    give.actor = actor;
    give.target = &target;
    give.engine = engine_reclaim_unit;
    give.context = &ask;
    return finish(state, give_order(*seat, give));
}

int method_build(lua_State* state) {
    UnitRef actor{};
    if (check_unit(state, 1, "build", &actor) != 0)
        return 0;
    uint16_t type = 0;
    if (!resolve_unit_type(state, 2, "build", &type))
        return 0;
    oa::FixedVec3 at{};
    if (!read_position(state, 3, "build", &at))
        return 0;
    uint8_t facing = 0;
    if (!read_facing(state, 4, "build", &facing))
        return 0;
    bool queued = false;
    if (!read_flag(state, 5, "build", &queued))
        return 0;
    Seat* seat = charged_seat(state, "build");
    BuildAsk ask{type, point_from(at), queued, facing};
    Give give;
    give.actor = actor;
    give.check_type = true;
    give.type = type;
    give.engine = engine_build;
    give.context = &ask;
    return finish(state, give_order(*seat, give));
}

int method_queue(lua_State* state) {
    UnitRef actor{};
    if (check_unit(state, 1, "queue", &actor) != 0)
        return 0;
    uint16_t type = 0;
    if (!resolve_unit_type(state, 2, "queue", &type))
        return 0;
    lua_Integer count = 1;
    if (!lua_isnoneornil(state, 3)) {
        if (lua_isinteger(state, 3) == 0)
            return luaL_error(state, "queue expects a count from 1 to 1000");
        count = lua_tointeger(state, 3);
        if (count < 1 || count > 1000)
            return luaL_error(state, "queue expects a count from 1 to 1000");
    }
    Seat* seat = charged_seat(state, "queue");
    CountAsk ask{type, static_cast<int32_t>(count)};
    Give give;
    give.actor = actor;
    give.check_type = true;
    give.type = type;
    give.engine = engine_queue;
    give.context = &ask;
    return finish(state, give_order(*seat, give));
}

int method_requeue(lua_State* state) {
    UnitRef actor{};
    if (check_unit(state, 1, "requeue", &actor) != 0)
        return 0;
    uint16_t type = 0;
    if (!resolve_unit_type(state, 2, "requeue", &type))
        return 0;
    if (lua_isinteger(state, 3) == 0)
        return luaL_error(state, "requeue expects a non-zero delta from -1000 to 1000");
    const lua_Integer delta = lua_tointeger(state, 3);
    if (delta == 0 || delta < -1000 || delta > 1000)
        return luaL_error(state, "requeue expects a non-zero delta from -1000 to 1000");
    Seat* seat = charged_seat(state, "requeue");
    CountAsk ask{type, static_cast<int32_t>(delta)};
    Give give;
    give.actor = actor;
    give.check_type = true;
    give.type = type;
    give.engine = engine_requeue;
    give.context = &ask;
    return finish(state, give_order(*seat, give));
}

int method_stop(lua_State* state) {
    UnitRef actor{};
    if (check_unit(state, 1, "stop", &actor) != 0)
        return 0;
    Seat* seat = charged_seat(state, "stop");
    Give give;
    give.actor = actor;
    give.engine = engine_stop;
    return finish(state, give_order(*seat, give));
}

int method_on(lua_State* state) {
    UnitRef actor{};
    if (check_unit(state, 1, "on", &actor) != 0)
        return 0;
    bool want = false;
    if (!read_required_bool(state, 2, "on", &want))
        return 0;
    Seat* seat = charged_seat(state, "on");
    Give give;
    give.actor = actor;
    give.skip = activation_already;
    give.engine = engine_on;
    give.context = &want;
    return finish(state, give_order(*seat, give));
}

int method_cloak(lua_State* state) {
    UnitRef actor{};
    if (check_unit(state, 1, "cloak", &actor) != 0)
        return 0;
    bool on = false;
    if (!read_required_bool(state, 2, "cloak", &on))
        return 0;
    Seat* seat = charged_seat(state, "cloak");
    Give give;
    give.actor = actor;
    give.engine = engine_cloak;
    give.context = &on;
    return finish(state, give_order(*seat, give));
}

int method_self_destruct(lua_State* state) {
    UnitRef actor{};
    if (check_unit(state, 1, "self_destruct", &actor) != 0)
        return 0;
    Seat* seat = charged_seat(state, "self_destruct");
    Give give;
    give.actor = actor;
    give.engine = engine_destruct;
    return finish(state, give_order(*seat, give));
}

const char* stance_move_name(lua_State* state, int table, int* value) {
    lua_getfield(state, table, "move");
    if (lua_isnoneornil(state, -1)) {
        lua_pop(state, 1);
        return nullptr;
    }
    if (lua_type(state, -1) != LUA_TSTRING) {
        luaL_error(state, "stance expects \"hold\", \"manoeuvre\" or \"roam\"");
        return nullptr;
    }
    const char* word = lua_tostring(state, -1);
    if (word != nullptr && std::strcmp(word, "hold") == 0)
        *value = 0;
    else if (word != nullptr && std::strcmp(word, "manoeuvre") == 0)
        *value = 1;
    else if (word != nullptr && std::strcmp(word, "roam") == 0)
        *value = 2;
    else
        luaL_error(state, "stance expects \"hold\", \"manoeuvre\" or \"roam\"");
    lua_pop(state, 1);
    return word;
}

const char* stance_fire_name(lua_State* state, int table, int* value) {
    lua_getfield(state, table, "fire");
    if (lua_isnoneornil(state, -1)) {
        lua_pop(state, 1);
        return nullptr;
    }
    if (lua_type(state, -1) != LUA_TSTRING) {
        luaL_error(state, "stance expects \"hold\", \"return\" or \"at_will\"");
        return nullptr;
    }
    const char* word = lua_tostring(state, -1);
    if (word != nullptr && std::strcmp(word, "hold") == 0)
        *value = 0;
    else if (word != nullptr && std::strcmp(word, "return") == 0)
        *value = 1;
    else if (word != nullptr && std::strcmp(word, "at_will") == 0)
        *value = 2;
    else
        luaL_error(state, "stance expects \"hold\", \"return\" or \"at_will\"");
    lua_pop(state, 1);
    return word;
}

void write_standing(oa::Unit& unit, bool has_move, int move_value, bool has_fire, int fire_value) {
    if (has_move) {
        unit.flags = (unit.flags & ~OA_UNIT_FLAG_MOVE_ORDER_MASK) |
                     (static_cast<uint32_t>(move_value) << OA_UNIT_FLAG_MOVE_ORDER_SHIFT);
    }
    if (has_fire) {
        unit.flags = (unit.flags & ~OA_UNIT_FLAG_FIRE_ORDER_MASK) |
                     (static_cast<uint32_t>(fire_value) << OA_UNIT_FLAG_FIRE_ORDER_SHIFT);
    }
}

int method_stance(lua_State* state) {
    UnitRef actor{};
    if (check_unit(state, 1, "stance", &actor) != 0)
        return 0;
    if (lua_type(state, 2) != LUA_TTABLE)
        return luaL_error(state, "stance expects a table");
    const int table = lua_absindex(state, 2);
    int move_value = 0;
    int fire_value = 0;
    const bool has_move = stance_move_name(state, table, &move_value) != nullptr;
    const bool has_fire = stance_fire_name(state, table, &fire_value) != nullptr;
    lua_getfield(state, table, "direct");
    bool direct = false;
    if (!lua_isnoneornil(state, -1)) {
        if (lua_type(state, -1) != LUA_TBOOLEAN)
            return luaL_error(state, "stance expects true or false");
        direct = lua_toboolean(state, -1) != 0;
    }
    lua_pop(state, 1);
    Seat* seat = seat_of(state);
    if (seat == nullptr)
        return luaL_error(state, "stance has no seat");
    // The privilege is checked before the unit, so a seat that is not
    // Classic is refused even when the unit would have been too.
    if (direct && (seat->match == nullptr || !seat_is_classic(*seat->match, seat->player)))
        return luaL_error(state, "stance: direct writes are for the Classic brain only");
    script_host::charge_work(state, order_charge);
    if (direct) {
        if (refuse_actor(*seat, actor))
            return finish(state, false);
        oa::Unit* unit = oa::world_unit_at(&seat->match->state(), actor.slot);
        if (unit == nullptr) {
            count(*seat, &OrderTally::by_engine);
            return finish(state, false);
        }
        write_standing(*unit, has_move, move_value, has_fire, fire_value);
        return finish(state, true);
    }
    bool given = true;
    if (has_move) {
        CountAsk ask{
            static_cast<uint16_t>(sim::match_runtime::standing_move_order_kind), move_value
        };
        Give give;
        give.actor = actor;
        give.engine = engine_state;
        give.context = &ask;
        given = give_order(*seat, give);
    }
    if (given && has_fire) {
        CountAsk ask{
            static_cast<uint16_t>(sim::match_runtime::standing_fire_order_kind), fire_value
        };
        Give give;
        give.actor = actor;
        give.engine = engine_state;
        give.context = &ask;
        given = give_order(*seat, give);
    }
    return finish(state, given);
}

void push_tally(lua_State* state, const OrderTally& tally) {
    lua_createtable(state, 0, 7);
    const auto put = [state](const char* key, uint32_t value) {
        lua_pushinteger(state, static_cast<lua_Integer>(value));
        lua_setfield(state, -2, key);
    };
    put("given", tally.given);
    put("not_own", tally.not_own);
    put("dead", tally.dead);
    put("not_seen", tally.not_seen);
    put("not_buildable", tally.not_buildable);
    put("over_limit", tally.over_limit);
    put("by_engine", tally.by_engine);
}

int brain_order_counts(lua_State* state) {
    bool totals = false;
    if (!lua_isnoneornil(state, 1)) {
        const char* which = lua_type(state, 1) == LUA_TSTRING ? lua_tostring(state, 1) : nullptr;
        if (which == nullptr || std::strcmp(which, "match") != 0)
            return luaL_error(state, "order_counts expects \"match\"");
        totals = true;
    }
    const Seat* seat = seat_of(state);
    if (seat == nullptr)
        return luaL_error(state, "order_counts has no seat");
    push_tally(state, totals ? seat->match_orders : seat->turn_orders);
    return 1;
}

} // extern "C"

// The own-class thunk returns nil before the method runs, so a dead or
// foreign unit would not be counted and would not pay the order charge.
// These methods take Access::always and do that check themselves.
void add_order_methods(lua_State* state) {
    add_unit_method(state, "move", Access::always, method_move);
    add_unit_method(state, "patrol", Access::always, method_patrol);
    add_unit_method(state, "attack", Access::always, method_attack);
    add_unit_method(state, "attack_ground", Access::always, method_attack_ground);
    add_unit_method(state, "attack_point", Access::always, method_attack_point);
    add_unit_method(state, "dgun", Access::always, method_dgun);
    add_unit_method(state, "guard", Access::always, method_guard);
    add_unit_method(state, "repair", Access::always, method_repair);
    add_unit_method(state, "help", Access::always, method_help);
    add_unit_method(state, "reclaim", Access::always, method_reclaim);
    add_unit_method(state, "capture", Access::always, method_capture);
    add_unit_method(state, "build", Access::always, method_build);
    add_unit_method(state, "queue", Access::always, method_queue);
    add_unit_method(state, "requeue", Access::always, method_requeue);
    add_unit_method(state, "load", Access::always, method_load);
    add_unit_method(state, "unload", Access::always, method_unload);
    add_unit_method(state, "stop", Access::always, method_stop);
    add_unit_method(state, "on", Access::always, method_on);
    add_unit_method(state, "cloak", Access::always, method_cloak);
    add_unit_method(state, "self_destruct", Access::always, method_self_destruct);
    add_unit_method(state, "stance", Access::always, method_stance);
}

} // namespace

void begin_order_turn(Seat& seat) noexcept {
    seat.turn_orders = {};
    seat.orders_this_turn = 0;
}

void install_orders(lua_State* state, Seat& seat) {
    store_seat(state, seat);
    add_order_methods(state);
    lua_getglobal(state, "brain");
    if (lua_type(state, -1) != LUA_TTABLE) {
        lua_pop(state, 1);
        lua_newtable(state);
    }
    lua_pushcfunction(state, brain_order_counts);
    lua_setfield(state, -2, "order_counts");
    lua_setglobal(state, "brain");
    install_site_functions(state, seat);
}

} // namespace oa::sim::brains
