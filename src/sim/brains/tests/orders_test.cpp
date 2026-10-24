// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Orders a seated brain gives, beside the same calls made from C++.
// The made-up brain is order-giver. Refusals are counted and never raised.

#include "match_fixture.hpp"

#include "internal.hpp"

#include "oa/base/game_loop.hpp"
#include "oa/core/unit.h"
#include "oa/core/unit_def.h"
#include "oa/core/world.h"
#include "oa/data/mission_types.hpp"
#include "oa/sim/brains/classic.hpp"
#include "oa/sim/brains/handles.hpp"
#include "oa/sim/brains/seat.hpp"
#include "oa/sim/gameplay_input/order_cursor.hpp"
#include "oa/sim/match_runtime/attack_orders.hpp"
#include "oa/sim/state_hash.hpp"

#include "lua.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace brains = oa::sim::brains;
namespace host = oa::data::script_host;
namespace runtime = oa::sim::match_runtime;

using Issue = std::function<bool(runtime::Match&, uint16_t, uint16_t)>;

struct Cast {
    uint16_t vehicle{};
    uint16_t enemy{};
    uint16_t frame{};
    uint16_t silo{};
    uint16_t flyer{};
};

struct SeatRequest {
    bool full_awareness{};
    bool classic{};
};

struct Flag {
    const char* name{};
    bool present{};
    bool value{};
};

struct TextFlag {
    const char* name{};
    std::string text{};
};

struct Tally {
    uint32_t given{};
    uint32_t not_own{};
    uint32_t dead{};
    uint32_t not_seen{};
    uint32_t not_buildable{};
    uint32_t over_limit{};
    uint32_t by_engine{};
};

struct CountQuery {
    bool match_totals{};
    bool failed{};
    Tally tally{};
};

struct Chunk {
    const char* source{};
};

struct PushCast {
    brains::Seat* seat{};
    Cast cast{};
};

int32_t whole_fixed(int32_t world_units) {
    return static_cast<int32_t>(static_cast<int64_t>(world_units) * int64_t{65536});
}

sim::ground_orders::Point world_point(int32_t x, int32_t z) {
    return {whole_fixed(x), 0, whole_fixed(z)};
}

void advance(Fixture& fixture) {
    ++fixture.match->state().game.tick;
    fixture.match->tick();
}

std::string load_giver() {
    const std::string path = std::string(OA_ORDER_GIVER_DIR) + "/brain.lua";
    std::ifstream input(path);
    if (!input)
        throw std::runtime_error("missing order-giver");
    std::string text;
    std::string line;
    while (std::getline(input, line)) {
        text.append(line);
        text.push_back('\n');
    }
    if (text.empty())
        throw std::runtime_error("empty order-giver");
    return text;
}

void ready_lists(Fixture& fixture) {
    CHECK(sim::ai::configure_match_computer_players(*fixture.match, profile, build_lists, false));
    sim::ai::prepare_match_computer_players(*fixture.match);
}

void arm_type(Fixture& fixture, Type type, bool fly) {
    fixture.defs[type].can_attack = true;
    fixture.defs[type].can_fly = fly;
    fixture.defs[type].weapon1 = "NUKE";
    oa::UnitDef& def = fixture.match->state().unit_defs[type];
    def.abilities |= OA_UNIT_DEF_ABILITY_CAN_ATTACK;
    if (fly)
        def.flags |= OA_UNIT_DEF_FLAG_CAN_FLY;
}

Cast populate(Fixture& fixture) {
    arm_type(fixture, vehicle, false);
    arm_type(fixture, missile, true);
    ready_lists(fixture);
    Cast cast;
    auto& mover = fixture.spawn(computer, vehicle, 20, 20);
    mover.record.flags |= OA_UNIT_FLAG_HAS_WEAPONS;
    cast.vehicle = mover.unit_index;
    auto& foe = fixture.spawn(0, vehicle, 30, 30);
    foe.record.flags |= OA_UNIT_FLAG_HAS_WEAPONS;
    cast.enemy = foe.unit_index;
    cast.frame = fixture.spawn(computer, collector, 12, 12).unit_index;
    cast.silo = fixture.spawn(computer, silo, 16, 24).unit_index;
    auto& flyer = fixture.spawn(computer, missile, 22, 18);
    flyer.record.flags |= OA_UNIT_FLAG_HAS_WEAPONS;
    cast.flyer = flyer.unit_index;
    return cast;
}

brains::Seat* seat_for(Fixture& fixture) {
    const auto& held = fixture.match->seat_state();
    CHECK(static_cast<bool>(held));
    auto* seated = static_cast<brains::Brains*>(held.get());
    CHECK(seated != nullptr);
    CHECK(seated->seats[computer] != nullptr);
    return seated->seats[computer].get();
}

host::Sandbox* seat_giver(Fixture& fixture, const std::string& script, SeatRequest request) {
    const auto bytes =
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(script.data()), script.size()};
    const brains::PackageFile file{"brain.lua", bytes};
    const std::span<const brains::PackageFile> files(&file, 1);
    const std::array<char, 65> fingerprint = brains::classic_fingerprint(files);
    brains::SeatSpec spec;
    spec.player = computer;
    spec.id = "order-giver";
    spec.name = "Order Giver";
    spec.version = "1";
    spec.entry = "brain.lua";
    spec.level = "hard";
    spec.difficulty = OA_DIFFICULTY_HARD;
    spec.full_awareness = request.full_awareness;
    spec.files = files;
    if (request.classic)
        spec.classic_fingerprint_for_tests = fingerprint.data();
    const brains::SeatOutcome outcome = brains::seat_brain(*fixture.match, spec);
    if (outcome.result != brains::SeatResult::seated)
        throw std::runtime_error(std::string("seat refused: ") + outcome.reason.data());
    if (request.classic)
        CHECK(brains::seat_is_classic(*fixture.match, computer));
    else
        CHECK(!brains::seat_is_classic(*fixture.match, computer));
    host::Sandbox* sandbox = brains::seat_sandbox(*fixture.match, computer);
    CHECK(sandbox != nullptr);
    return sandbox;
}

int push_chunk(void* context, lua_State* state) {
    const auto* chunk = static_cast<const Chunk*>(context);
    if (chunk == nullptr || chunk->source == nullptr)
        return luaL_error(state, "missing probe");
    if (luaL_loadstring(state, chunk->source) != 0) {
        const char* message = lua_tostring(state, -1);
        if (message == nullptr)
            message = "probe failed to load";
        return luaL_error(state, "%s", message);
    }
    return 0;
}

void run_chunk(host::Sandbox& sandbox, const std::string& source) {
    Chunk chunk{source.c_str()};
    host::CallHooks hooks;
    hooks.context = &chunk;
    hooks.push = push_chunk;
    host::ScriptError error;
    if (!sandbox.call("probe", hooks, error))
        throw std::runtime_error(error.message.empty() ? "probe failed" : error.message);
}

void push_cast(void* context, lua_State* state) {
    auto* held = static_cast<PushCast*>(context);
    if (held == nullptr || held->seat == nullptr || held->seat->match == nullptr)
        return;
    const auto put = [&](const char* name, uint16_t slot) {
        if (slot == 0)
            return;
        brains::push_unit(state, *held->seat, brains::unit_ref(*held->seat->match, slot));
        lua_setglobal(state, name);
    };
    put("unit", held->cast.vehicle);
    put("other", held->cast.enemy);
    put("frame", held->cast.frame);
    put("silo", held->cast.silo);
    put("flyer", held->cast.flyer);
    put("mover", held->cast.vehicle);
}

void publish_cast(host::Sandbox& sandbox, brains::Seat& seat, const Cast& cast) {
    PushCast held{&seat, cast};
    host::ScriptError error;
    if (!sandbox.run_host("push-cast", &held, push_cast, error))
        throw std::runtime_error(error.message.empty() ? "push cast failed" : error.message);
}

void read_flag(void* context, lua_State* state) {
    auto* flag = static_cast<Flag*>(context);
    lua_getglobal(state, flag->name);
    flag->present = lua_isnil(state, -1) == 0;
    flag->value = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
}

bool flag_of(host::Sandbox& sandbox, const char* name) {
    Flag flag{name, false, false};
    host::ScriptError error;
    CHECK(sandbox.run_host("read-flag", &flag, read_flag, error));
    CHECK(flag.present);
    return flag.value;
}

void read_text(void* context, lua_State* state) {
    auto* flag = static_cast<TextFlag*>(context);
    lua_getglobal(state, flag->name);
    std::size_t length = 0;
    const char* text = lua_tolstring(state, -1, &length);
    if (text != nullptr)
        flag->text.assign(text, length);
    lua_pop(state, 1);
}

std::string text_of(host::Sandbox& sandbox, const char* name) {
    TextFlag flag{name, {}};
    host::ScriptError error;
    CHECK(sandbox.run_host("read-text", &flag, read_text, error));
    return flag.text;
}

void read_integer_field(lua_State* state, const char* key, uint32_t* out) {
    lua_getfield(state, -1, key);
    *out = static_cast<uint32_t>(lua_tointeger(state, -1));
    lua_pop(state, 1);
}

void read_counts(void* context, lua_State* state) {
    auto* query = static_cast<CountQuery*>(context);
    lua_getglobal(state, "brain");
    lua_getfield(state, -1, "order_counts");
    if (query->match_totals)
        lua_pushliteral(state, "match");
    if (lua_pcall(state, query->match_totals ? 1 : 0, 1, 0) != 0) {
        query->failed = true;
        lua_pop(state, 2);
        return;
    }
    read_integer_field(state, "given", &query->tally.given);
    read_integer_field(state, "not_own", &query->tally.not_own);
    read_integer_field(state, "dead", &query->tally.dead);
    read_integer_field(state, "not_seen", &query->tally.not_seen);
    read_integer_field(state, "not_buildable", &query->tally.not_buildable);
    read_integer_field(state, "over_limit", &query->tally.over_limit);
    read_integer_field(state, "by_engine", &query->tally.by_engine);
    lua_pop(state, 2);
}

Tally counts_of(host::Sandbox& sandbox, bool match_totals) {
    CountQuery query;
    query.match_totals = match_totals;
    host::ScriptError error;
    CHECK(sandbox.run_host("read-counts", &query, read_counts, error));
    CHECK(!query.failed);
    return query.tally;
}

using Record = runtime::Match::OrderRecordView;

std::vector<Record> queue_of(runtime::Match& match, uint16_t unit, bool secondary) {
    std::array<Record, 24> records{};
    const std::size_t count = match.queue_records(unit, secondary, records.data(), records.size());
    CHECK(count < records.size());
    return {records.begin(), records.begin() + static_cast<std::ptrdiff_t>(count)};
}

bool same_order(const Record& left, const Record& right) {
    return left.kind == right.kind && left.target == right.target && left.point == right.point &&
           left.parameter_1 == right.parameter_1 && left.parameter_2 == right.parameter_2 &&
           left.facing == right.facing && left.flags == right.flags;
}

void expect_same_queues(runtime::Match& left, runtime::Match& right, uint16_t unit) {
    for (const bool secondary : {false, true}) {
        const std::vector<Record> one = queue_of(left, unit, secondary);
        const std::vector<Record> two = queue_of(right, unit, secondary);
        CHECK(one.size() == two.size());
        for (std::size_t row = 0; row < one.size(); ++row)
            CHECK(same_order(one[row], two[row]));
    }
}

void expect_same_future(Fixture& left, Fixture& right) {
    advance(left);
    advance(right);
    CHECK(left.match->random_state() == right.match->random_state());
    const oa::base::game_loop::Timing timing{};
    const uint64_t left_hash = oa::sim::trace::match_state_hash(*left.match, timing, 0, 0, nullptr);
    const uint64_t right_hash =
        oa::sim::trace::match_state_hash(*right.match, timing, 0, 0, nullptr);
    CHECK(left_hash == right_hash);
}

bool kept(runtime::Match& match, sim::simulation_state::Order& order) {
    return !match.order_refused(order);
}

/// The built-in player's attack on a point, copied from its order host.
bool attack_point(runtime::Match& match, uint16_t unit, const oa::FixedVec3& at) {
    const oa::World& world = match.state();
    const oa::Unit* actor = oa::world_unit_at(&world, unit);
    if (actor == nullptr)
        return false;
    const auto order = sim::gameplay_input::unit_order(
        world, sim::gameplay_input::OrderCommand::attack, *actor, nullptr, &at, {}
    );
    const uint8_t kind =
        data::mission_types::index_for_name(sim::gameplay_input::unit_order_name(order));
    if (kind == data::mission_types::unknown_mission)
        return false;
    const sim::ground_orders::Point point{at.x, at.y, at.z};
    return !match.order_refused(match.issue_order(unit, kind, false, 0, &point, 0, 0));
}

void twin(
    const std::string& script,
    bool aware,
    const std::string& body,
    const Issue& issue,
    uint16_t Cast::* actor,
    bool expect_given,
    uint16_t Cast::* target = &Cast::enemy
) {
    Fixture left(0, true, {}, 1);
    Fixture right(0, true, {}, 1);
    const Cast left_cast = populate(left);
    const Cast right_cast = populate(right);
    host::Sandbox* sandbox = seat_giver(left, script, SeatRequest{aware, false});
    seat_giver(right, script, SeatRequest{aware, false});
    publish_cast(*sandbox, *seat_for(left), left_cast);
    run_chunk(*sandbox, body);
    const bool gave = flag_of(*sandbox, "gave");
    bool from_cpp = false;
    try {
        from_cpp = issue(*right.match, right_cast.*actor, right_cast.*target);
    } catch (...) {
        from_cpp = false;
    }
    CHECK(gave == from_cpp);
    if (expect_given)
        CHECK(gave);
    expect_same_queues(*left.match, *right.match, left_cast.*actor);
    expect_same_future(left, right);
}

void each_method_matches_the_match_call(const std::string& script) {
    const sim::ground_orders::Point at = world_point(400, 360);
    const oa::FixedVec3 at_fixed{at[0], at[1], at[2]};
    const auto point_body = [](const char* method, bool queued) {
        return std::string("gave = unit:") + method + "({x = 400, z = 360}, " +
               (queued ? "true" : "false") + ")\n";
    };
    const auto target_body = [](const char* method, bool queued) {
        return std::string("gave = unit:") + method + "(other, " + (queued ? "true" : "false") +
               ")\n";
    };
    for (const bool queued : {false, true}) {
        twin(
            script,
            true,
            point_body("move", queued),
            [=](runtime::Match& match, uint16_t unit, uint16_t) {
                return kept(match, match.issue_ground_move(unit, at, queued));
            },
            &Cast::vehicle,
            true
        );
        twin(
            script,
            true,
            point_body("patrol", queued),
            [=](runtime::Match& match, uint16_t unit, uint16_t) {
                return kept(match, match.issue_patrol(unit, at, queued));
            },
            &Cast::vehicle,
            true
        );
        twin(
            script,
            true,
            target_body("attack", queued),
            [=](runtime::Match& match, uint16_t unit, uint16_t target) {
                return match.issue_attack_command(unit, target, queued, nullptr);
            },
            &Cast::vehicle,
            true
        );
        twin(
            script,
            true,
            point_body("attack_ground", queued),
            [=](runtime::Match& match, uint16_t unit, uint16_t) {
                return match.issue_attack_ground(unit, at, queued) != nullptr;
            },
            &Cast::vehicle,
            true
        );
        twin(
            script,
            true,
            std::string("gave = flyer:attack_ground({x = 400, z = 360}, ") +
                (queued ? "true" : "false") + ")\n",
            [=](runtime::Match& match, uint16_t unit, uint16_t) {
                return match.issue_attack_ground(unit, at, queued) != nullptr;
            },
            &Cast::flyer,
            true
        );
        twin(
            script,
            true,
            point_body("dgun", queued),
            [=](runtime::Match& match, uint16_t unit, uint16_t) {
                return kept(match, match.issue_attack_special(unit, at, queued, 0));
            },
            &Cast::vehicle,
            true
        );
        twin(
            script,
            true,
            std::string("gave = unit:dgun(other, ") + (queued ? "true" : "false") + ")\n",
            [=](runtime::Match& match, uint16_t unit, uint16_t target) {
                const oa::Unit* aimed = oa::world_unit_at(&match.state(), target);
                CHECK(aimed != nullptr);
                const sim::ground_orders::Point where{
                    aimed->position.x, aimed->position.y, aimed->position.z
                };
                return kept(match, match.issue_attack_special(unit, where, queued, target));
            },
            &Cast::vehicle,
            true
        );
        twin(
            script,
            true,
            target_body("guard", queued),
            [=](runtime::Match& match, uint16_t unit, uint16_t target) {
                return kept(match, match.issue_guard(unit, target, queued));
            },
            &Cast::vehicle,
            true
        );
        twin(
            script,
            true,
            target_body("repair", queued),
            [=](runtime::Match& match, uint16_t unit, uint16_t target) {
                return kept(match, match.issue_repair(unit, target, queued));
            },
            &Cast::vehicle,
            true
        );
        twin(
            script,
            true,
            std::string("gave = unit:help(frame, ") + (queued ? "true" : "false") + ")\n",
            [=](runtime::Match& match, uint16_t unit, uint16_t target) {
                return kept(match, match.issue_help_build(unit, target, queued));
            },
            &Cast::vehicle,
            true,
            &Cast::frame
        );
        twin(
            script,
            true,
            target_body("capture", queued),
            [=](runtime::Match& match, uint16_t unit, uint16_t target) {
                return kept(match, match.issue_capture(unit, target, queued));
            },
            &Cast::vehicle,
            true
        );
        twin(
            script,
            true,
            target_body("load", queued),
            [=](runtime::Match& match, uint16_t unit, uint16_t target) {
                return kept(match, match.issue_load(unit, target, queued));
            },
            &Cast::vehicle,
            true
        );
        twin(
            script,
            true,
            std::string("gave = unit:unload({x = 400, z = 360}, ") + (queued ? "true" : "false") +
                ")\n",
            [=](runtime::Match& match, uint16_t unit, uint16_t) {
                return kept(match, match.issue_unload(unit, at, queued));
            },
            &Cast::vehicle,
            true
        );
        twin(
            script,
            true,
            target_body("reclaim", queued),
            [=](runtime::Match& match, uint16_t unit, uint16_t target) {
                return kept(match, match.issue_reclaim(unit, target, queued));
            },
            &Cast::vehicle,
            true
        );
        twin(
            script,
            true,
            std::string("gave = unit:reclaim({x = 400, z = 360}, ") + (queued ? "true" : "false") +
                ")\n",
            [=](runtime::Match& match, uint16_t unit, uint16_t) {
                return kept(match, match.issue_feature_reclaim(unit, at, queued));
            },
            &Cast::vehicle,
            true
        );
        twin(
            script,
            true,
            std::string("gave = unit:build(\"ARMSOLAR\", {x = 480, z = 480}, 1, ") +
                (queued ? "true" : "false") + ")\n",
            [=](runtime::Match& match, uint16_t unit, uint16_t) {
                return kept(
                    match,
                    match.issue_mobile_build(unit, collector, world_point(480, 480), queued, 1)
                );
            },
            &Cast::vehicle,
            true
        );
    }
    twin(
        script,
        true,
        "gave = unit:attack_point({x = 400, z = 360})\n",
        [=](runtime::Match& match, uint16_t unit, uint16_t) {
            return attack_point(match, unit, at_fixed);
        },
        &Cast::vehicle,
        true
    );
    twin(
        script,
        true,
        "gave = flyer:attack_point({x = 400, z = 360})\n",
        [=](runtime::Match& match, uint16_t unit, uint16_t) {
            return attack_point(match, unit, at_fixed);
        },
        &Cast::flyer,
        true
    );
    twin(
        script,
        true,
        "gave = unit:stop()\n",
        [](runtime::Match& match, uint16_t unit, uint16_t) {
            return kept(match, match.issue_stop(unit));
        },
        &Cast::vehicle,
        true
    );
    twin(
        script,
        true,
        "gave = unit:cloak(true)\n",
        [](runtime::Match& match, uint16_t unit, uint16_t) {
            return kept(match, match.issue_cloak(unit, true));
        },
        &Cast::vehicle,
        true
    );
    twin(
        script,
        true,
        "gave = unit:self_destruct()\n",
        [](runtime::Match& match, uint16_t unit, uint16_t) {
            match.toggle_self_destruct(std::span<const uint16_t>(&unit, 1));
            return true;
        },
        &Cast::vehicle,
        true
    );
    twin(
        script,
        true,
        "gave = unit:stance({move = \"roam\", fire = \"at_will\"})\n",
        [](runtime::Match& match, uint16_t unit, uint16_t) {
            const bool move =
                kept(match, match.issue_state_order(unit, runtime::standing_move_order_kind, 2));
            if (!move)
                return false;
            return kept(match, match.issue_state_order(unit, runtime::standing_fire_order_kind, 2));
        },
        &Cast::vehicle,
        true
    );
    twin(
        script,
        true,
        "gave = silo:queue(\"ARMMAKENUKE\", 3)\n",
        [](runtime::Match& match, uint16_t unit, uint16_t) {
            match.queue_factory_build(unit, missile, 3);
            return true;
        },
        &Cast::silo,
        true
    );
    twin(
        script,
        true,
        "gave = silo:queue(\"ARMMAKENUKE\")\n"
        "gave = silo:requeue(\"ARMMAKENUKE\", -1)\n",
        [](runtime::Match& match, uint16_t unit, uint16_t) {
            match.queue_factory_build(unit, missile, 1);
            const uint8_t kind = data::mission_types::index_for_name("BuildingBuild");
            if (kind == data::mission_types::unknown_mission)
                return false;
            match.change_queued_count(unit, kind, missile, -1);
            return true;
        },
        &Cast::silo,
        true
    );
}

void on_toggles_only_when_needed(const std::string& script) {
    Fixture left(0, true, {}, 1);
    Fixture right(0, true, {}, 1);
    const Cast left_cast = populate(left);
    const Cast right_cast = populate(right);
    host::Sandbox* sandbox = seat_giver(left, script, {});
    seat_giver(right, script, {});
    publish_cast(*sandbox, *seat_for(left), left_cast);
    const oa::Unit* before = oa::world_unit_at(&left.match->state(), left_cast.silo);
    CHECK(before != nullptr);
    const bool active = (before->state_flags & OA_UNIT_STATE_ACTIVE) != 0;
    const char* word = active ? "false" : "true";
    run_chunk(
        *sandbox, std::string("gave = silo:on(") + word + ")\ngave_again = silo:on(" + word + ")\n"
    );
    CHECK(flag_of(*sandbox, "gave"));
    CHECK(flag_of(*sandbox, "gave_again"));
    const oa::Unit* after = oa::world_unit_at(&left.match->state(), left_cast.silo);
    CHECK(after != nullptr);
    CHECK(((after->state_flags & OA_UNIT_STATE_ACTIVE) != 0) != active);
    right.match->toggle_activation(right_cast.silo, OA_UNIT_STATE_ACTIVE);
    expect_same_queues(*left.match, *right.match, left_cast.silo);
    const Tally turn = counts_of(*sandbox, false);
    CHECK(turn.given == 1);
    CHECK(turn.by_engine == 0);
    expect_same_future(left, right);
}

void direct_stance_is_classics_alone(const std::string& script) {
    const char* classic_body =
        "unit:move({x = 400, z = 360})\n"
        "local ok, err = pcall(function()\n"
        "  gave = unit:stance({move = \"hold\", fire = \"hold\", direct = true})\n"
        "end)\n"
        "raised = not ok\n"
        "raised_text = tostring(err)\n";
    const char* raised_body =
        "local ok, err = pcall(function()\n"
        "  gave = unit:stance({move = \"hold\", fire = \"hold\", direct = true})\n"
        "end)\n"
        "raised = not ok\n"
        "raised_text = tostring(err)\n";

    Fixture classic_match(0, true, {}, 1);
    const Cast classic_cast = populate(classic_match);
    host::Sandbox* classic_box = seat_giver(classic_match, script, SeatRequest{false, true});
    publish_cast(*classic_box, *seat_for(classic_match), classic_cast);
    const std::size_t queued_before =
        queue_of(*classic_match.match, classic_cast.vehicle, false).size();
    run_chunk(*classic_box, classic_body);
    CHECK(!flag_of(*classic_box, "raised"));
    CHECK(flag_of(*classic_box, "gave"));
    CHECK(!classic_box->failed());
    const oa::Unit* written =
        oa::world_unit_at(&classic_match.match->state(), classic_cast.vehicle);
    CHECK(written != nullptr);
    const uint32_t move_order =
        (written->flags & OA_UNIT_FLAG_MOVE_ORDER_MASK) >> OA_UNIT_FLAG_MOVE_ORDER_SHIFT;
    const uint32_t fire_order =
        (written->flags & OA_UNIT_FLAG_FIRE_ORDER_MASK) >> OA_UNIT_FLAG_FIRE_ORDER_SHIFT;
    CHECK(move_order == 0);
    CHECK(fire_order == 0);
    CHECK(queue_of(*classic_match.match, classic_cast.vehicle, false).size() == queued_before + 1);
    const Tally classic_counts = counts_of(*classic_box, false);
    CHECK(classic_counts.given == 1);

    const auto expect_raise = [&](bool aware) {
        Fixture other(0, true, {}, 1);
        const Cast cast = populate(other);
        host::Sandbox* sandbox = seat_giver(other, script, SeatRequest{aware, false});
        publish_cast(*sandbox, *seat_for(other), cast);
        const oa::Unit* prior = oa::world_unit_at(&other.match->state(), cast.vehicle);
        CHECK(prior != nullptr);
        const uint32_t flags = prior->flags;
        run_chunk(*sandbox, raised_body);
        CHECK(flag_of(*sandbox, "raised"));
        CHECK(!sandbox->failed());
        const std::string message = text_of(*sandbox, "raised_text");
        CHECK(
            message.find("stance: direct writes are for the Classic brain only") !=
            std::string::npos
        );
        const oa::Unit* same = oa::world_unit_at(&other.match->state(), cast.vehicle);
        CHECK(same != nullptr);
        CHECK(same->flags == flags);
        CHECK(queue_of(*other.match, cast.vehicle, false).empty());
    };
    expect_raise(false);
    expect_raise(true);
}

// A05 reads a live enemy the seat can see. The fixture enemy stands inside
// the vehicle's sight, so hide one with the cloak bit unit_visible refuses.
void hide_from_sight(Fixture& fixture, uint16_t slot) {
    sim::unit_spawn::Slot& hidden = fixture.match->spawn_slot(slot);
    hidden.record.state_flags =
        static_cast<uint8_t>(hidden.record.state_flags | OA_UNIT_STATE_CLOAKED);
    CHECK(!fixture.match->unit_visible(computer, slot));
}

void refusals_are_counted(const std::string& script) {
    Fixture owned(0, true, {}, 1);
    const Cast cast = populate(owned);
    host::Sandbox* sandbox = seat_giver(owned, script, {});
    publish_cast(*sandbox, *seat_for(owned), cast);
    run_chunk(*sandbox, "gave = other:move({x = 10, z = 10})\n");
    CHECK(!flag_of(*sandbox, "gave"));
    CHECK(counts_of(*sandbox, false).not_own == 1);
    CHECK(queue_of(*owned.match, cast.enemy, false).empty());

    const brains::UnitRef living = brains::unit_ref(*owned.match, cast.frame);
    owned.match->kill_unit(cast.frame, static_cast<uint8_t>(runtime::DeathKind::dismissed));
    CHECK(!brains::unit_current(*owned.match, living));
    run_chunk(*sandbox, "gave = frame:move({x = 10, z = 10})\n");
    CHECK(!flag_of(*sandbox, "gave"));
    CHECK(counts_of(*sandbox, false).dead == 1);

    // `other` is in sight, so A05 would give this attack. Cloak it first: the
    // seat cannot see a cloaked enemy, and the refusal is still counted.
    hide_from_sight(owned, cast.enemy);
    run_chunk(*sandbox, "gave = unit:attack(other)\n");
    CHECK(!flag_of(*sandbox, "gave"));
    CHECK(counts_of(*sandbox, false).not_seen == 1);
    CHECK(queue_of(*owned.match, cast.vehicle, false).empty());

    run_chunk(*sandbox, "gave = unit:build(\"ARMSILO\", {x = 480, z = 480})\n");
    CHECK(!flag_of(*sandbox, "gave"));
    CHECK(counts_of(*sandbox, false).not_buildable == 1);

    run_chunk(*sandbox, "gave = frame:stop()\n");
    CHECK(counts_of(*sandbox, false).dead == 2);

    Fixture seen(0, true, {}, 1);
    const Cast seen_cast = populate(seen);
    host::Sandbox* aware = seat_giver(seen, script, SeatRequest{true, false});
    publish_cast(*aware, *seat_for(seen), seen_cast);
    // The same cloaked enemy is not in sight. Full awareness is what gives
    // the attack; line of sight would refuse it, as the case above counts.
    hide_from_sight(seen, seen_cast.enemy);
    run_chunk(*aware, "gave = unit:attack(other)\n");
    CHECK(flag_of(*aware, "gave"));
    CHECK(counts_of(*aware, false).given == 1);
    CHECK(counts_of(*aware, false).not_seen == 0);
    CHECK(!queue_of(*seen.match, seen_cast.vehicle, false).empty());

    Fixture stuck(0, true, {}, 1);
    const Cast stuck_cast = populate(stuck);
    host::Sandbox* boxed = seat_giver(stuck, script, {});
    publish_cast(*boxed, *seat_for(stuck), stuck_cast);
    Fixture twin_stuck(0, true, {}, 1);
    const Cast twin_cast = populate(twin_stuck);
    const sim::ground_orders::Point at = world_point(10, 10);
    bool from_cpp = false;
    try {
        from_cpp = kept(
            *twin_stuck.match, twin_stuck.match->issue_ground_move(twin_cast.frame, at, false)
        );
    } catch (...) {
        from_cpp = false;
    }
    run_chunk(*boxed, "gave = frame:move({x = 10, z = 10})\n");
    CHECK(flag_of(*boxed, "gave") == from_cpp);
    CHECK(!flag_of(*boxed, "gave"));
    CHECK(counts_of(*boxed, false).by_engine == 1);
    expect_same_queues(*stuck.match, *twin_stuck.match, stuck_cast.frame);
}

void the_turn_stops_at_4096(const std::string& script) {
    Fixture fixture(0, true, {}, 1);
    const Cast cast = populate(fixture);
    host::Sandbox* sandbox = seat_giver(fixture, script, {});
    publish_cast(*sandbox, *seat_for(fixture), cast);
    run_chunk(
        *sandbox,
        "local n = 0\n"
        "for _ = 1, 4096 do\n"
        "  if unit:move({x = 400, z = 360}, false) then\n"
        "    n = n + 1\n"
        "  end\n"
        "end\n"
        "extra = unit:move({x = 400, z = 360}, false)\n"
        "given_n = n\n"
    );
    CHECK(!flag_of(*sandbox, "extra"));
    const Tally turn = counts_of(*sandbox, false);
    const Tally match = counts_of(*sandbox, true);
    CHECK(turn.given == 4096);
    CHECK(turn.over_limit == 1);
    CHECK(turn.by_engine == 0);
    CHECK(match.given == turn.given);
    CHECK(match.over_limit == turn.over_limit);
    CHECK(match.not_own == 0);
    CHECK(match.dead == 0);
    CHECK(match.not_seen == 0);
    CHECK(match.not_buildable == 0);
    advance(fixture);
    const Tally next = counts_of(*sandbox, false);
    const Tally kept_match = counts_of(*sandbox, true);
    CHECK(next.given == 0);
    CHECK(next.over_limit == 0);
    CHECK(kept_match.given == 4096);
    CHECK(kept_match.over_limit == 1);
}

void counts_agree_until_the_next_turn(const std::string& script) {
    Fixture fixture(0, true, {}, 1);
    const Cast cast = populate(fixture);
    host::Sandbox* sandbox = seat_giver(fixture, script, {});
    publish_cast(*sandbox, *seat_for(fixture), cast);
    run_chunk(*sandbox, "gave = unit:move({x = 400, z = 360})\n");
    CHECK(flag_of(*sandbox, "gave"));
    const Tally turn = counts_of(*sandbox, false);
    const Tally match = counts_of(*sandbox, true);
    CHECK(turn.given == 1);
    CHECK(match.given == 1);
    run_chunk(
        *sandbox,
        "local ok = pcall(function() brain.order_counts(\"turn\") end)\n"
        "bad_which = not ok\n"
    );
    CHECK(flag_of(*sandbox, "bad_which"));
    CHECK(!sandbox->failed());
    advance(fixture);
    CHECK(counts_of(*sandbox, false).given == 0);
    CHECK(counts_of(*sandbox, true).given == 1);
}

void a_move_runs_on_the_next_sweep(const std::string& script) {
    Fixture fixture(0, true, {}, 1);
    auto& mover = fixture.spawn(computer, vehicle, 40, 40);
    const int32_t before = mover.record.position.x;
    host::Sandbox* sandbox = seat_giver(fixture, script, {});
    PushCast held{seat_for(fixture), {}};
    held.cast.vehicle = mover.unit_index;
    host::ScriptError error;
    CHECK(sandbox->run_host("push-mover", &held, push_cast, error));
    run_chunk(*sandbox, "move_once = true\nmove_x = 800\nmove_z = 648\n");
    advance(fixture);
    CHECK(mover.record.position.x == before);
    CHECK(flag_of(*sandbox, "move_result"));
    advance(fixture);
    CHECK(mover.record.position.x != before);
}

void argument_errors_raise(const std::string& script) {
    Fixture fixture(0, true, {}, 1);
    const Cast cast = populate(fixture);
    host::Sandbox* sandbox = seat_giver(fixture, script, {});
    publish_cast(*sandbox, *seat_for(fixture), cast);
    run_chunk(
        *sandbox,
        "local ok, err = pcall(function() unit:move(1) end)\n"
        "raised = not ok\n"
        "raised_text = tostring(err)\n"
    );
    CHECK(flag_of(*sandbox, "raised"));
    CHECK(text_of(*sandbox, "raised_text").find("move expects a position") != std::string::npos);
    CHECK(counts_of(*sandbox, false).given == 0);
    CHECK(!sandbox->failed());
}

} // namespace

int main() {
    try {
        const std::string script = load_giver();
        each_method_matches_the_match_call(script);
        on_toggles_only_when_needed(script);
        direct_stance_is_classics_alone(script);
        refusals_are_counted(script);
        the_turn_stops_at_4096(script);
        counts_agree_until_the_next_turn(script);
        a_move_runs_on_the_next_sweep(script);
        argument_errors_raise(script);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "sim-brains-orders: %s\n", ex.what());
        return 1;
    }
    return 0;
}
