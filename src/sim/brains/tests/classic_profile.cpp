// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Classic's profile passes and knowledge reads, beside the built-in player.
// The built-in player keeps the record. Classic reads owned counts, the
// builder count and the base position from that record.

#include "classic_harness.hpp"

#include "internal.hpp"

#include "oa/data/match_rules/difficulty_names.hpp"
#include "oa/test/game_assets.hpp"
#include "oa/test/game_data.hpp"

#include "lua.hpp"

#include <cstdint>
#include <cstdio>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using oa::test::classic_harness::ClassicBind;
using oa::test::classic_harness::PackageBytes;
using oa::test::classic_harness::capture_random;
using oa::test::classic_harness::copy_catalog;
using oa::test::classic_harness::knowledge_link;
using oa::test::classic_harness::open_classic;
using oa::test::classic_harness::play_classic;
using oa::test::classic_harness::recording_setup;
using oa::test::classic_harness::refresh_knowledge;
using oa::test::computer_fake::ARMCK;
using oa::test::computer_fake::ARMCOM;
using oa::test::computer_fake::ARMFIG;
using oa::test::computer_fake::ARMLAB;
using oa::test::computer_fake::ARMLLT;
using oa::test::computer_fake::ARMMEX;
using oa::test::computer_fake::ARMPT;
using oa::test::computer_fake::ARMPW;
using oa::test::computer_fake::ARMSOLAR;
using oa::test::computer_fake::CORMEX;
using oa::test::computer_fake::Fake;
using oa::test::computer_fake::TYPE_COUNT;
using oa::test::computer_fake::describe_catalog;
using oa::test::computer_fake::host_for;
using oa::test::computer_fake::kBuildLists;

namespace {

using oa::sim::ai::ComputerHost;
using oa::sim::ai::ComputerKnowledge;
using oa::sim::ai::ComputerPlayers;
using oa::sim::ai::KnowledgeTally;

constexpr int kTypeCap = 16;
constexpr uint32_t kProfileSeed = 0xC030C03u;

struct HeldPlayers {
    ComputerPlayers state{};

    HeldPlayers() = default;
    HeldPlayers(const HeldPlayers&) = delete;
    HeldPlayers& operator=(const HeldPlayers&) = delete;

    ~HeldPlayers() { oa::sim::ai::computer_players_release(&state); }
};

/// Clears the knowledge link after the seat and the players are gone.
struct LinkGuard {
    LinkGuard() = default;
    LinkGuard(const LinkGuard&) = delete;
    LinkGuard& operator=(const LinkGuard&) = delete;

    ~LinkGuard() { knowledge_link = {}; }
};

struct RecordGuard {
    explicit RecordGuard(oa::sim::match_runtime::SetUpRecord* record) { recording_setup = record; }

    RecordGuard(const RecordGuard&) = delete;
    RecordGuard& operator=(const RecordGuard&) = delete;

    ~RecordGuard() { recording_setup = nullptr; }
};

struct CallStatus {
    bool failed{};
    char message[240]{};
};

struct Snapshot {
    int32_t type_count{};
    int32_t weight[kTypeCap]{};
    int32_t limits[kTypeCap]{};
    int32_t weight_locked[kTypeCap]{};
    int32_t limit_locked[kTypeCap]{};
    int32_t base_weight[kTypeCap]{};
    int32_t owned[kTypeCap]{};
    int32_t builders{};
    int32_t base_x{};
    int32_t base_y{};
    int32_t base_z{};
    int32_t plan{};
    int32_t grids_ready{};
    int32_t land[5]{};
    int32_t water[5]{};
    int32_t build_count[kTypeCap]{};
    int32_t has_build_list[kTypeCap]{};
    std::vector<int32_t> build_ids[kTypeCap];
    std::vector<std::pair<int32_t, int32_t>> metal;
    bool read_weights{true};
    bool read_builds{};
    bool read_knowledge{};
    bool read_grids{};
    bool read_metal{};
    CallStatus status{};
};

struct Directive {
    uint16_t type{};
    const char* text{};
};

void fail(const std::string& text) {
    throw std::runtime_error(text);
}

void check(bool ok, const std::string& text) {
    if (!ok)
        fail(text);
}

void note(CallStatus* status, const char* text) {
    if (status == nullptr || status->failed)
        return;
    status->failed = true;
    std::size_t index = 0;
    if (text != nullptr) {
        while (index + 1 < sizeof status->message && text[index] != '\0') {
            status->message[index] = text[index];
            ++index;
        }
    }
    status->message[index] = '\0';
}

void check_call(const CallStatus& status) {
    if (status.failed)
        fail(status.message[0] != '\0' ? status.message : "the seat call failed");
}

const char* difficulty_word(int32_t difficulty) {
    if (difficulty == OA_DIFFICULTY_EASY)
        return "easy";
    if (difficulty == OA_DIFFICULTY_HARD)
        return "hard";
    return "medium";
}

uint32_t next_bits(uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

void copy_word(char* dest, std::size_t room, const char* text) {
    std::size_t index = 0;
    if (text != nullptr) {
        while (index + 1 < room && text[index] != '\0') {
            dest[index] = text[index];
            ++index;
        }
    }
    if (room > 0)
        dest[index] = '\0';
}

int64_t take_integer(lua_State* state, CallStatus* status) {
    int64_t value = 0;
    if (lua_isinteger(state, -1) == 0)
        note(status, "a Classic table held a value that is not an integer");
    else
        value = static_cast<int64_t>(lua_tointeger(state, -1));
    lua_pop(state, 1);
    return value;
}

int64_t raw_integer(lua_State* state, CallStatus* status, int table, lua_Integer key) {
    if (status->failed)
        return 0;
    lua_rawgeti(state, table, key);
    return take_integer(state, status);
}

int64_t named_integer(lua_State* state, CallStatus* status, int table, const char* name) {
    if (status->failed)
        return 0;
    lua_getfield(state, table, name);
    return take_integer(state, status);
}

void read_array(
    lua_State* state, CallStatus* status, int seat, const char* name, int32_t* dest, int32_t count
) {
    if (status->failed)
        return;
    lua_getfield(state, seat, name);
    if (lua_type(state, -1) != LUA_TTABLE) {
        note(status, "a Classic table is missing");
        lua_pop(state, 1);
        return;
    }
    const int table = lua_gettop(state);
    for (int32_t type_index = 0; type_index < count; ++type_index)
        dest[type_index] = static_cast<int32_t>(raw_integer(state, status, table, type_index));
    lua_pop(state, 1);
}

void read_grid(lua_State* state, CallStatus* status, int seat, const char* name, int32_t* dest) {
    if (status->failed)
        return;
    lua_getfield(state, seat, name);
    if (lua_type(state, -1) != LUA_TTABLE) {
        note(status, "a build grid is missing");
        lua_pop(state, 1);
        return;
    }
    const int grid = lua_gettop(state);
    dest[0] = static_cast<int32_t>(named_integer(state, status, grid, "step_x"));
    dest[1] = static_cast<int32_t>(named_integer(state, status, grid, "step_z"));
    dest[2] = static_cast<int32_t>(named_integer(state, status, grid, "phase_x"));
    dest[3] = static_cast<int32_t>(named_integer(state, status, grid, "phase_z"));
    dest[4] = static_cast<int32_t>(named_integer(state, status, grid, "margin"));
    lua_pop(state, 1);
}

bool push_required(lua_State* state, CallStatus* status, const char* module, const char* field) {
    lua_getglobal(state, "require");
    lua_pushstring(state, module);
    if (lua_pcall(state, 1, 1, 0) != LUA_OK) {
        note(status, lua_tostring(state, -1));
        return false;
    }
    lua_getfield(state, -1, field);
    lua_remove(state, -2);
    if (lua_type(state, -1) != LUA_TFUNCTION) {
        note(status, "a Classic function is missing");
        return false;
    }
    return true;
}

int64_t call_seat_integer(
    lua_State* state,
    CallStatus* status,
    int seat,
    const char* module,
    const char* field,
    lua_Integer argument,
    bool with_argument
) {
    if (status->failed)
        return 0;
    if (!push_required(state, status, module, field))
        return 0;
    lua_pushvalue(state, seat);
    int args = 1;
    if (with_argument) {
        lua_pushinteger(state, argument);
        args = 2;
    }
    if (lua_pcall(state, args, 1, 0) != LUA_OK) {
        note(status, lua_tostring(state, -1));
        return 0;
    }
    return take_integer(state, status);
}

void read_base(lua_State* state, CallStatus* status, int seat, Snapshot* shot) {
    if (status->failed)
        return;
    if (!push_required(state, status, "knowledge", "base"))
        return;
    lua_pushvalue(state, seat);
    if (lua_pcall(state, 1, 1, 0) != LUA_OK) {
        note(status, lua_tostring(state, -1));
        return;
    }
    if (lua_type(state, -1) != LUA_TTABLE) {
        note(status, "knowledge.base did not return a point");
        lua_pop(state, 1);
        return;
    }
    const int point = lua_gettop(state);
    shot->base_x = static_cast<int32_t>(named_integer(state, status, point, "x"));
    shot->base_y = static_cast<int32_t>(named_integer(state, status, point, "y"));
    shot->base_z = static_cast<int32_t>(named_integer(state, status, point, "z"));
    lua_pop(state, 1);
}

void read_builds(lua_State* state, CallStatus* status, int seat, Snapshot* shot) {
    if (status->failed)
        return;
    lua_getfield(state, seat, "types");
    if (lua_type(state, -1) != LUA_TTABLE) {
        note(status, "the type table is missing");
        lua_pop(state, 1);
        return;
    }
    const int types = lua_gettop(state);
    for (int32_t type_index = 1; type_index < shot->type_count; ++type_index) {
        lua_rawgeti(state, types, type_index);
        if (lua_type(state, -1) != LUA_TTABLE) {
            note(status, "a type row is missing");
            lua_pop(state, 1);
            return;
        }
        const int row = lua_gettop(state);
        const int32_t count =
            static_cast<int32_t>(named_integer(state, status, row, "build_count"));
        shot->build_count[type_index] = count;
        shot->has_build_list[type_index] =
            static_cast<int32_t>(named_integer(state, status, row, "has_build_list"));
        lua_getfield(state, row, "build_ids");
        if (lua_type(state, -1) != LUA_TTABLE) {
            note(status, "a build list is missing");
            lua_pop(state, 2);
            return;
        }
        const int list = lua_gettop(state);
        shot->build_ids[type_index].clear();
        for (int32_t slot = 1; slot <= count; ++slot) {
            shot->build_ids[type_index].push_back(
                static_cast<int32_t>(raw_integer(state, status, list, slot))
            );
        }
        lua_pop(state, 2);
    }
    lua_pop(state, 1);
}

void read_metal(lua_State* state, CallStatus* status, int seat, Snapshot* shot) {
    if (status->failed)
        return;
    lua_getfield(state, seat, "metal");
    if (lua_type(state, -1) != LUA_TTABLE) {
        note(status, "the metal list is missing");
        lua_pop(state, 1);
        return;
    }
    const int list = lua_gettop(state);
    const lua_Integer count = lua_rawlen(state, list);
    shot->metal.clear();
    for (lua_Integer index = 1; index <= count; ++index) {
        lua_rawgeti(state, list, index);
        if (lua_type(state, -1) != LUA_TTABLE) {
            note(status, "a metal spot is missing");
            lua_pop(state, 2);
            return;
        }
        const int spot = lua_gettop(state);
        const auto x = static_cast<int32_t>(named_integer(state, status, spot, "x"));
        const auto z = static_cast<int32_t>(named_integer(state, status, spot, "z"));
        shot->metal.emplace_back(x, z);
        lua_pop(state, 1);
    }
    lua_pop(state, 1);
}

void dump_work(void* context, lua_State* state) {
    auto* shot = static_cast<Snapshot*>(context);
    if (shot == nullptr)
        return;
    const int seat = lua_gettop(state);
    if (lua_type(state, seat) != LUA_TTABLE) {
        note(&shot->status, "the seat table is missing");
        return;
    }
    lua_getfield(state, seat, "types");
    if (lua_type(state, -1) != LUA_TTABLE) {
        note(&shot->status, "the type table is missing");
        lua_pop(state, 1);
        return;
    }
    const int listed = static_cast<int>(lua_rawlen(state, -1));
    lua_pop(state, 1);
    shot->type_count = listed + 1;
    if (shot->type_count > kTypeCap) {
        note(&shot->status, "the type table is longer than the comparison allows");
        return;
    }
    if (shot->read_weights) {
        read_array(state, &shot->status, seat, "weight_percent", shot->weight, shot->type_count);
        read_array(state, &shot->status, seat, "limits", shot->limits, shot->type_count);
        read_array(
            state, &shot->status, seat, "weight_locked", shot->weight_locked, shot->type_count
        );
        read_array(
            state, &shot->status, seat, "limit_locked", shot->limit_locked, shot->type_count
        );
        read_array(state, &shot->status, seat, "base_weights", shot->base_weight, shot->type_count);
        shot->plan = static_cast<int32_t>(named_integer(state, &shot->status, seat, "plan"));
    }
    if (shot->read_grids) {
        shot->grids_ready =
            static_cast<int32_t>(named_integer(state, &shot->status, seat, "grids_ready"));
        read_grid(state, &shot->status, seat, "land_grid", shot->land);
        read_grid(state, &shot->status, seat, "water_grid", shot->water);
    }
    if (shot->read_builds)
        read_builds(state, &shot->status, seat, shot);
    if (shot->read_metal)
        read_metal(state, &shot->status, seat, shot);
    if (shot->read_knowledge) {
        for (int32_t type_index = 0; type_index < shot->type_count; ++type_index) {
            shot->owned[type_index] = static_cast<int32_t>(call_seat_integer(
                state, &shot->status, seat, "knowledge", "owned", type_index, true
            ));
        }
        shot->builders = static_cast<int32_t>(
            call_seat_integer(state, &shot->status, seat, "knowledge", "builders", 0, false)
        );
        read_base(state, &shot->status, seat, shot);
    }
}

Snapshot read_snapshot(oa::sim::brains::ToolSeat& seat, const Snapshot& request) {
    Snapshot shot;
    shot.read_weights = request.read_weights;
    shot.read_builds = request.read_builds;
    shot.read_knowledge = request.read_knowledge;
    shot.read_grids = request.read_grids;
    shot.read_metal = request.read_metal;
    oa::data::script_host::ScriptError error;
    if (!seat.with_seat(&shot, dump_work, error))
        fail(oa::data::script_host::describe(error));
    check_call(shot.status);
    return shot;
}

struct PassAsk {
    const char* difficulty{};
    bool fresh{};
    bool reload{};
    bool restart{};
    int32_t plan{1};
    CallStatus status{};
};

void pass_work(void* context, lua_State* state) {
    auto* ask = static_cast<PassAsk*>(context);
    const int seat = lua_gettop(state);
    if (lua_type(state, seat) != LUA_TTABLE) {
        note(&ask->status, "the seat table is missing");
        return;
    }
    if (ask->difficulty != nullptr)
        oa::sim::brains::set_hidden_difficulty(state, ask->difficulty);
    if (ask->restart) {
        if (!push_required(state, &ask->status, "brain", "start"))
            return;
        lua_getfield(state, seat, "game");
        lua_getfield(state, seat, "me");
        if (lua_pcall(state, 2, 0, 0) != LUA_OK)
            note(&ask->status, lua_tostring(state, -1));
        return;
    }
    if (ask->fresh) {
        if (!push_required(state, &ask->status, "knowledge", "reset"))
            return;
        lua_pushvalue(state, seat);
        if (lua_pcall(state, 1, 0, 0) != LUA_OK) {
            note(&ask->status, lua_tostring(state, -1));
            return;
        }
        lua_pushinteger(state, 1);
        lua_setfield(state, seat, "plan");
        if (!push_required(state, &ask->status, "profile", "apply_all"))
            return;
        lua_pushvalue(state, seat);
        if (lua_pcall(state, 1, 0, 0) != LUA_OK)
            note(&ask->status, lua_tostring(state, -1));
        return;
    }
    if (ask->reload) {
        lua_pushinteger(state, ask->plan);
        lua_setfield(state, seat, "plan");
        if (!push_required(state, &ask->status, "profile", "reload"))
            return;
        lua_pushvalue(state, seat);
        if (lua_pcall(state, 1, 0, 0) != LUA_OK)
            note(&ask->status, lua_tostring(state, -1));
    }
}

void run_pass(oa::sim::brains::ToolSeat& seat, const PassAsk& request) {
    PassAsk ask = request;
    oa::data::script_host::ScriptError error;
    if (!seat.with_seat(&ask, pass_work, error))
        fail(oa::data::script_host::describe(error));
    check_call(ask.status);
}

struct NumberAsk {
    const char* name{};
    double factor{};
    lua_Integer limit{};
    bool weight{};
    CallStatus status{};
};

void number_work(void* context, lua_State* state) {
    auto* ask = static_cast<NumberAsk*>(context);
    const int seat = lua_gettop(state);
    const char* field = ask->weight ? "weight" : "limit";
    if (!push_required(state, &ask->status, "profile", field))
        return;
    lua_pushvalue(state, seat);
    lua_pushstring(state, ask->name);
    if (ask->weight)
        lua_pushnumber(state, ask->factor);
    else
        lua_pushinteger(state, ask->limit);
    if (lua_pcall(state, 3, 0, 0) != LUA_OK)
        note(&ask->status, lua_tostring(state, -1));
}

void apply_console(
    oa::sim::brains::ToolSeat& seat, bool weight, const char* name, double factor, lua_Integer limit
) {
    NumberAsk ask;
    ask.name = name;
    ask.factor = factor;
    ask.limit = limit;
    ask.weight = weight;
    oa::data::script_host::ScriptError error;
    if (!seat.with_seat(&ask, number_work, error))
        fail(oa::data::script_host::describe(error));
    check_call(ask.status);
}

void expect_equal(long long got, long long want, const std::string& label) {
    if (got == want)
        return;
    fail(label + " got " + std::to_string(got) + " want " + std::to_string(want));
}

void compare_weights(
    const Snapshot& shot, const ComputerKnowledge& knowledge, uint32_t type_count
) {
    check(shot.type_count == static_cast<int32_t>(type_count), "type count");
    for (uint32_t type_index = 0; type_index < type_count; ++type_index) {
        const std::string name = "type " + std::to_string(type_index);
        expect_equal(
            shot.weight[type_index], knowledge.weight_percent[type_index], name + " weight"
        );
        expect_equal(shot.limits[type_index], knowledge.limits[type_index], name + " limit");
        expect_equal(
            shot.weight_locked[type_index],
            knowledge.weight_locked[type_index] != 0 ? 1 : 0,
            name + " weight lock"
        );
        expect_equal(
            shot.limit_locked[type_index],
            knowledge.limit_locked[type_index] != 0 ? 1 : 0,
            name + " limit lock"
        );
        expect_equal(
            shot.base_weight[type_index], knowledge.base_weights[type_index], name + " base weight"
        );
    }
}

void compare_knowledge(
    const Snapshot& shot, const ComputerKnowledge& knowledge, uint32_t type_count
) {
    for (uint32_t type_index = 0; type_index < type_count; ++type_index) {
        expect_equal(
            shot.owned[type_index],
            knowledge.owned_counts[type_index],
            "owned type " + std::to_string(type_index)
        );
    }
    expect_equal(shot.builders, knowledge.builder_count, "builders");
    expect_equal(shot.base_x, knowledge.base_position.x, "base x");
    expect_equal(shot.base_y, knowledge.base_position.y, "base y");
    expect_equal(shot.base_z, knowledge.base_position.z, "base z");
}

void compare_grids(const Snapshot& shot, const ComputerKnowledge& knowledge) {
    const int32_t land[] = {
        knowledge.land_grid.step_x,
        knowledge.land_grid.step_z,
        knowledge.land_grid.phase_x,
        knowledge.land_grid.phase_z,
        knowledge.land_grid.margin
    };
    const int32_t water[] = {
        knowledge.water_grid.step_x,
        knowledge.water_grid.step_z,
        knowledge.water_grid.phase_x,
        knowledge.water_grid.phase_z,
        knowledge.water_grid.margin
    };
    const char* land_name[] = {
        "land step x", "land step z", "land phase x", "land phase z", "land margin"
    };
    const char* water_name[] = {
        "water step x", "water step z", "water phase x", "water phase z", "water margin"
    };
    for (int index = 0; index < 5; ++index) {
        expect_equal(shot.land[index], land[index], land_name[index]);
        expect_equal(shot.water[index], water[index], water_name[index]);
    }
}

void compare_builds(const Snapshot& shot, const ComputerPlayers& state) {
    for (uint32_t type_index = 1; type_index < state.type_count && type_index < kTypeCap;
         ++type_index) {
        const auto& type = state.types[type_index];
        const std::string name = "type " + std::to_string(type_index);
        expect_equal(
            shot.has_build_list[type_index], type.has_build_list, name + " has_build_list"
        );
        expect_equal(shot.build_count[type_index], type.build_count, name + " build count");
        check(shot.build_ids[type_index].size() == type.build_count, name + " build id count");
        for (uint16_t slot = 0; slot < type.build_count; ++slot) {
            expect_equal(
                shot.build_ids[type_index][slot],
                type.build_ids[slot],
                name + " build id " + std::to_string(slot)
            );
        }
    }
}

void compare_metal(const Snapshot& shot, const ComputerKnowledge& knowledge) {
    expect_equal(
        static_cast<long long>(shot.metal.size()), knowledge.metal_spot_count, "metal count"
    );
    for (uint32_t index = 0; index < knowledge.metal_spot_count && index < shot.metal.size();
         ++index) {
        expect_equal(shot.metal[index].first, knowledge.metal_spots[index].cell_x, "metal x");
        expect_equal(shot.metal[index].second, knowledge.metal_spots[index].cell_z, "metal z");
    }
}

struct Prepared {
    LinkGuard guard{};
    Fake fake{};
    HeldPlayers players{};
    oa::data::match_rules::MatchRules rules{};
    oa::data::brain_facts::GameFacts facts{};
    std::vector<oa::data::unit_definitions::UnitDefinition> definitions;
    oa::sim::match_runtime::SetUpRecord setup{};
    ComputerHost host{};
    std::optional<oa::sim::brains::ToolSeat> seat;
    std::optional<oa::sim::brains::ToolSeat> other;
};

void set_directive(ComputerPlayers& state, uint16_t type, const char* text) {
    state.types[type].flags |= OA_UNIT_DEF_FLAG_DOWNLOADABLE;
    copy_word(state.types[type].ai_directives, sizeof state.types[type].ai_directives, text);
}

void seat_classic(
    Prepared& prep,
    const PackageBytes& package,
    const ClassicBind& bind,
    const oa::sim::match_runtime::SetUpRecord* setup,
    bool second
) {
    auto opened = open_classic(package, prep.host, prep.facts, prep.definitions, setup, &bind);
    if (second)
        prep.other = std::move(opened);
    else
        prep.seat = std::move(opened);
}

/// One built-in player and one Classic seat over the same catalog and profile.
void prepare(
    Prepared& prep,
    const PackageBytes& package,
    const char* text,
    const uint16_t* runs,
    uint32_t run_length,
    uint32_t kept,
    int32_t game_difficulty,
    int32_t seat_difficulty,
    uint8_t player,
    bool capture,
    bool handed,
    const oa::data::match_rules::AiDifficultyNames* names
) {
    describe_catalog(prep.players.state);
    copy_catalog(prep.fake, prep.players.state, prep.definitions);
    check(
        oa::sim::ai::computer_players_configure(
            &prep.players.state,
            text == nullptr ? "" : text,
            std::span<const uint16_t>(runs, run_length)
        ),
        "configure"
    );
    if (names != nullptr) {
        prep.rules.ai.difficulty_names = *names;
        prep.players.state.rules.match = &prep.rules;
        oa::data::mod_profile::ModProfile mod;
        mod.rules.ai.difficulty_names = *names;
        prep.facts.mod.profile = std::move(mod);
    }
    prep.host = host_for(prep.fake);
    prep.host.difficulty = game_difficulty;
    prep.facts.difficulty = game_difficulty;
    if (text != nullptr && text[0] != '\0') {
        prep.facts.ai_profile.path = "ai/profile.txt";
        prep.facts.ai_profile.text = text;
    }
    if (capture) {
        RecordGuard recording(&prep.setup);
        const auto saved = prep.host.random;
        prep.host.random = capture_random;
        check(
            oa::sim::ai::computer_players_initialize(&prep.players.state, prep.host), "initialize"
        );
        prep.host.random = saved;
    } else {
        check(
            oa::sim::ai::computer_players_initialize(&prep.players.state, prep.host), "initialize"
        );
    }
    if (handed)
        prep.setup.handed_over = true;
    ClassicBind bind;
    bind.use_player = true;
    bind.player = player;
    bind.use_difficulty = true;
    bind.difficulty = seat_difficulty;
    bind.build_runs = runs;
    bind.build_run_length = run_length;
    bind.build_kept = kept;
    bind.knowledge_fake = &prep.fake;
    bind.knowledge_players = &prep.players.state;
    const oa::sim::match_runtime::SetUpRecord* setup = capture || handed ? &prep.setup : nullptr;
    seat_classic(prep, package, bind, setup, false);
}

void mark_players(Fake& fake, int computers) {
    fake.player(0, OA_PLAYER_STATUS_LOCAL, 0, 0);
    for (int index = 1; index <= computers; ++index)
        fake.player(index, OA_PLAYER_STATUS_COMPUTER, 0, 0);
}

int32_t effective_difficulty(const ComputerPlayers& state, uint8_t player, int32_t game) {
    return oa::sim::ai::computer_player_difficulty(&state, player, game);
}

void compare_seat_to_player(oa::sim::brains::ToolSeat& seat, Prepared& prep, uint8_t player) {
    Snapshot request;
    request.read_knowledge = true;
    const Snapshot shot = read_snapshot(seat, request);
    const auto* knowledge = oa::sim::ai::computer_player_knowledge(&prep.players.state, player);
    check(knowledge != nullptr, "the built-in player has a record");
    compare_weights(shot, *knowledge, prep.players.state.type_count);
    compare_knowledge(shot, *knowledge, prep.players.state.type_count);
    expect_equal(shot.plan, prep.players.state.plan_matches ? 1 : 0, "plan");
}

void fresh_at(oa::sim::brains::ToolSeat& seat, int32_t difficulty) {
    PassAsk ask;
    ask.fresh = true;
    ask.difficulty = difficulty_word(difficulty);
    run_pass(seat, ask);
}

void reload_at(oa::sim::brains::ToolSeat& seat, int32_t difficulty, int32_t plan) {
    PassAsk ask;
    ask.reload = true;
    ask.plan = plan;
    ask.difficulty = difficulty_word(difficulty);
    run_pass(seat, ask);
}

bool metal_cells[64][64]{};

bool test_metal_feature(void*, int32_t x, int32_t z) {
    if (x < 0 || z < 0 || x >= 64 || z >= 64)
        return false;
    return metal_cells[z][x];
}

void clear_metal() {
    for (auto& row : metal_cells)
        for (bool& cell : row)
            cell = false;
}

} // namespace

void test_build_list_limits(const PackageBytes& package) {
    constexpr uint32_t listed = 1100;
    const uint16_t cycle[] = {ARMMEX, ARMSOLAR, ARMLAB, ARMPW, ARMCK};
    std::vector<uint16_t> lists = {0, static_cast<uint16_t>(listed)};
    for (uint32_t index = 0; index < listed; ++index)
        lists.push_back(cycle[index % 5]);

    struct Case {
        oa::data::limits::BuildLists lists;
        uint32_t kept{};
    };

    const Case cases[] = {
        {{}, 31},
        {{36, oa::data::limits::BuildListOverflow::truncate}, 37},
        {{oa::data::limits::highest_build_list_copy, oa::data::limits::BuildListOverflow::truncate},
         oa::data::limits::highest_build_list_copy + 1},
        {{30, oa::data::limits::BuildListOverflow::dynamic},
         oa::data::limits::highest_build_list_copy + 1},
    };
    for (const Case& one : cases) {
        Prepared prep;
        mark_players(prep.fake, 1);
        prep.players.state.build_lists = one.lists;
        prepare(
            prep,
            package,
            "",
            lists.data(),
            static_cast<uint32_t>(lists.size()),
            one.kept,
            OA_DIFFICULTY_EASY,
            OA_DIFFICULTY_EASY,
            1,
            false,
            false,
            nullptr
        );
        Snapshot request;
        request.read_builds = true;
        request.read_weights = false;
        const Snapshot shot = read_snapshot(*prep.seat, request);
        compare_builds(shot, prep.players.state);
        expect_equal(shot.build_count[ARMCOM], one.kept, "ARMCOM entries kept");
        for (uint32_t index = 0; index < one.kept; ++index)
            expect_equal(shot.build_ids[ARMCOM][index], cycle[index % 5], "ARMCOM id order");
        expect_equal(shot.build_count[ARMLAB], 0, "other builders keep their own list");
    }
}

void test_whole_build_lists(const PackageBytes& package) {
    const uint16_t lists[] = {
        0,
        4,
        ARMSOLAR,
        ARMLLT,
        ARMLLT,
        static_cast<uint16_t>(TYPE_COUNT + 3),
        1,
        ARMPW,
        0,
        2,
        ARMPW,
        ARMLLT
    };
    Prepared prep;
    mark_players(prep.fake, 1);
    prepare(
        prep,
        package,
        "",
        lists,
        static_cast<uint32_t>(std::size(lists)),
        31,
        OA_DIFFICULTY_EASY,
        OA_DIFFICULTY_EASY,
        1,
        false,
        false,
        nullptr
    );
    Snapshot request;
    request.read_builds = true;
    const Snapshot shot = read_snapshot(*prep.seat, request);
    compare_builds(shot, prep.players.state);
    expect_equal(shot.build_count[ARMCOM], 3, "commander list length");
    expect_equal(shot.build_ids[ARMCOM][0], ARMSOLAR, "commander first id");
    expect_equal(shot.build_ids[ARMCOM][1], ARMLLT, "commander repeated id");
    expect_equal(shot.build_ids[ARMCOM][2], ARMLLT, "commander repeat kept");
    expect_equal(shot.build_count[ARMMEX], 0, "a type that builds nothing");
    expect_equal(shot.build_count[ARMLAB], 2, "lab list length");
    expect_equal(shot.build_ids[ARMLAB][0], ARMPW, "lab first id");
    expect_equal(shot.build_ids[ARMLAB][1], ARMLLT, "lab second id");
    expect_equal(shot.has_build_list[ARMCK], 1, "a builder the lists do not reach");
    expect_equal(shot.build_count[ARMCK], 0, "that builder's list is empty");
}

void test_profile(const PackageBytes& package) {
    const char* text = R"(// sample
plan easy
Weight ARM 0.2
Weight ARMLAB 0.5
Weight PLANT 2
Limit ARMMEX 8
limit level1 3
plan hard
limit ARMMEX 1
weight ARMSOLAR 0.1 # trailing comment
)";
    for (const int32_t difficulty : {OA_DIFFICULTY_EASY, OA_DIFFICULTY_HARD}) {
        Prepared prep;
        mark_players(prep.fake, 1);
        prepare(
            prep,
            package,
            text,
            kBuildLists,
            static_cast<uint32_t>(std::size(kBuildLists)),
            31,
            difficulty,
            difficulty,
            1,
            false,
            false,
            nullptr
        );
        Snapshot request;
        request.read_builds = true;
        request.read_knowledge = true;
        const Snapshot shot = read_snapshot(*prep.seat, request);
        const auto* knowledge = oa::sim::ai::computer_player_knowledge(&prep.players.state, 1);
        check(knowledge != nullptr, "profile record");
        compare_weights(shot, *knowledge, prep.players.state.type_count);
        compare_builds(shot, prep.players.state);
        compare_knowledge(shot, *knowledge, prep.players.state.type_count);
        if (difficulty == OA_DIFFICULTY_EASY) {
            expect_equal(shot.weight[ARMPW], 20, "category weight scales");
            expect_equal(shot.weight[ARMLAB], 10, "exact weight locks after category");
            expect_equal(shot.weight[CORMEX], 100, "other side untouched");
            expect_equal(shot.limits[ARMMEX], 8, "exact limit");
            expect_equal(shot.limit_locked[ARMMEX], 1, "exact limit locks");
            expect_equal(shot.limits[ARMSOLAR], 3, "category limit");
        } else {
            expect_equal(shot.weight[ARMPW], 100, "easy plan skipped on hard");
            expect_equal(shot.limits[ARMMEX], 1, "hard limit");
            expect_equal(shot.weight[ARMSOLAR], 10, "comment stripped");
        }
        expect_equal(shot.build_count[ARMCOM], 4, "side build list length");
        expect_equal(shot.build_ids[ARMCOM][0], ARMSOLAR, "side build list first");
        expect_equal(shot.build_ids[ARMCOM][2], ARMLAB, "side build list third");
        expect_equal(shot.build_ids[ARMCOM][3], CORMEX, "side build list fourth");
        expect_equal(shot.has_build_list[ARMCK], 1, "builder without list");
        expect_equal(shot.build_count[ARMCK], 0, "builder without list is empty");
        expect_equal(shot.base_weight[ARMLAB], 60, "lab base weight");
        expect_equal(shot.base_weight[ARMSOLAR], 40, "solar base weight");
        expect_equal(shot.base_weight[ARMCOM], 20, "commander base weight");
        expect_equal(shot.base_weight[ARMPW], 0, "kbot base weight");
        check(
            !prep.players.state.players[0].present && prep.players.state.players[1].present,
            "computer controller only"
        );
    }
}

void test_adopt_handed_player(const PackageBytes& package) {
    const char* text = "plan any\nweight ARM 0.5\n";
    Prepared prep;
    prep.fake.player(0, OA_PLAYER_STATUS_LOCAL, 1, 5);
    prep.fake.player(1, OA_PLAYER_STATUS_COMPUTER, 10, 30);
    describe_catalog(prep.players.state);
    copy_catalog(prep.fake, prep.players.state, prep.definitions);
    check(
        oa::sim::ai::computer_players_configure(
            &prep.players.state,
            text,
            std::span<const uint16_t>(kBuildLists, std::size(kBuildLists))
        ),
        "configure"
    );
    prep.host = host_for(prep.fake);
    prep.facts.difficulty = OA_DIFFICULTY_EASY;
    prep.facts.ai_profile.path = "ai/profile.txt";
    prep.facts.ai_profile.text = text;
    check(oa::sim::ai::computer_players_initialize(&prep.players.state, prep.host), "initialize");
    check(
        !prep.players.state.players[0].present && prep.players.state.players[1].present,
        "only the computer starts"
    );
    const auto kept = prep.players.state.players[1].knowledge.weight_percent[ARMPW];
    prep.fake.player(0, OA_PLAYER_STATUS_COMPUTER, 1, 5);
    oa::sim::ai::computer_players_adopt(&prep.players.state, prep.host, 0);
    check(prep.players.state.players[0].present != 0, "a handed player gets a controller");
    expect_equal(
        prep.players.state.players[0].knowledge.weight_percent[ARMPW],
        kept,
        "the profile applies once"
    );
    expect_equal(
        prep.players.state.players[1].knowledge.weight_percent[ARMPW],
        kept,
        "the other computer is kept"
    );
    oa::sim::ai::computer_players_adopt(&prep.players.state, prep.host, 0);
    check(prep.players.state.players[0].present != 0, "a second adopt leaves the controller");
    prep.fake.player(2, OA_PLAYER_STATUS_LOCAL, 40, 50);
    oa::sim::ai::computer_players_adopt(&prep.players.state, prep.host, 2);
    check(prep.players.state.players[2].present == 0, "a human is not adopted");

    prep.setup.handed_over = true;
    ClassicBind bind;
    bind.use_player = true;
    bind.player = 0;
    bind.use_difficulty = true;
    bind.difficulty = OA_DIFFICULTY_EASY;
    bind.build_runs = kBuildLists;
    bind.build_run_length = static_cast<uint32_t>(std::size(kBuildLists));
    bind.build_kept = 31;
    bind.knowledge_fake = &prep.fake;
    bind.knowledge_players = &prep.players.state;
    seat_classic(prep, package, bind, &prep.setup, false);
    Snapshot request;
    request.read_knowledge = true;
    const Snapshot shot = read_snapshot(*prep.seat, request);
    compare_weights(shot, prep.players.state.players[0].knowledge, prep.players.state.type_count);
    expect_equal(shot.weight[ARMPW], 50, "handed weight");
    expect_equal(kept, 50, "the computer already playing stays at one pass");
}

void test_downloadable_directives(const PackageBytes& package) {
    const char* text = "plan any\nlimit ARMCOM 2\n";
    Prepared prep;
    prep.fake.player(1, OA_PLAYER_STATUS_COMPUTER, 10, 30);
    prep.fake.player(2, OA_PLAYER_STATUS_COMPUTER, 31, 50);
    describe_catalog(prep.players.state);
    set_directive(prep.players.state, ARMFIG, "weight VTOL 0.5\nweight ARMPT 0.5\n");
    set_directive(prep.players.state, ARMCOM, "weight SHIP 0.5\nweight LEVEL1 0.5\n");
    copy_catalog(prep.fake, prep.players.state, prep.definitions);
    check(
        oa::sim::ai::computer_players_configure(
            &prep.players.state,
            text,
            std::span<const uint16_t>(kBuildLists, std::size(kBuildLists))
        ),
        "configure"
    );
    prep.host = host_for(prep.fake);
    prep.facts.difficulty = OA_DIFFICULTY_EASY;
    prep.facts.ai_profile.path = "ai/profile.txt";
    prep.facts.ai_profile.text = text;
    check(oa::sim::ai::computer_players_initialize(&prep.players.state, prep.host), "initialize");
    ClassicBind bind;
    bind.use_difficulty = true;
    bind.difficulty = OA_DIFFICULTY_EASY;
    bind.build_runs = kBuildLists;
    bind.build_run_length = static_cast<uint32_t>(std::size(kBuildLists));
    bind.build_kept = 31;
    bind.knowledge_fake = &prep.fake;
    bind.knowledge_players = &prep.players.state;
    bind.use_player = true;
    bind.player = 1;
    seat_classic(prep, package, bind, nullptr, false);
    bind.player = 2;
    seat_classic(prep, package, bind, nullptr, true);
    for (const uint8_t player : {uint8_t{1}, uint8_t{2}}) {
        oa::sim::brains::ToolSeat& seat = player == 1 ? *prep.seat : *prep.other;
        Snapshot request;
        const Snapshot shot = read_snapshot(seat, request);
        compare_weights(
            shot, prep.players.state.players[player].knowledge, prep.players.state.type_count
        );
        expect_equal(shot.weight[ARMFIG], 6, "category weight compounds once per pass and player");
        expect_equal(shot.limit_locked[ARMCOM], 1, "profile limit locks ARMCOM");
        expect_equal(shot.weight[ARMMEX], 25, "limit-locked type skipped by the limit passes");
        expect_equal(shot.weight[ARMPT], 25, "exact weight");
        expect_equal(shot.weight_locked[ARMPT], 1, "exact weight locks");
    }
}

void test_profile_reload(const PackageBytes& package) {
    const char* text = "weight ARMSOLAR 0.5\nplan easy\nlimit ARMMEX 4\n";
    Prepared prep;
    mark_players(prep.fake, 1);
    prep.fake.rolls.assign(64, 5);
    prepare(
        prep,
        package,
        text,
        kBuildLists,
        static_cast<uint32_t>(std::size(kBuildLists)),
        31,
        OA_DIFFICULTY_EASY,
        OA_DIFFICULTY_EASY,
        1,
        true,
        false,
        nullptr
    );
    Snapshot request;
    request.read_grids = true;
    request.read_knowledge = true;
    Snapshot shot = read_snapshot(*prep.seat, request);
    auto* knowledge = oa::sim::ai::computer_player_knowledge(&prep.players.state, 1);
    check(knowledge != nullptr, "reload record");
    compare_weights(shot, *knowledge, prep.players.state.type_count);
    expect_equal(shot.weight[ARMSOLAR], 50, "the leading line applies on the first load");
    expect_equal(shot.weight_locked[ARMSOLAR], 1, "the leading line locks");
    expect_equal(shot.limits[ARMMEX], 4, "easy plan limit");
    expect_equal(shot.limit_locked[ARMMEX], 1, "easy plan limit locks");
    expect_equal(shot.plan, 1, "the downloadable passes leave the plan flag set");
    expect_equal(shot.grids_ready, 0, "grids wait for the first turn");
    apply_console(*prep.seat, true, "ARMPW", 0.25, 0);
    apply_console(*prep.seat, false, "ARMLLT", 0, 2);
    oa::sim::ai::computer_apply_weight(&prep.players.state, 1, "ARMPW", 0.25F);
    oa::sim::ai::computer_apply_limit(&prep.players.state, 1, "ARMLLT", 2);
    shot = read_snapshot(*prep.seat, request);
    expect_equal(shot.weight[ARMPW], 25, "console weight");
    expect_equal(shot.limits[ARMLLT], 2, "console limit");
    const auto roll_before = prep.fake.roll;
    play_classic(*prep.seat, 1);
    expect_equal(
        static_cast<long long>(prep.fake.roll - roll_before),
        2,
        "grid seeds do not draw the shared stream"
    );
    shot = read_snapshot(*prep.seat, request);
    compare_grids(shot, *knowledge);
    expect_equal(shot.grids_ready, 1, "grids are seeded");
    const auto roll_seeded = prep.fake.roll;
    const int32_t land_step = shot.land[0];
    const int32_t water_phase = shot.water[3];
    PassAsk again;
    again.restart = true;
    run_pass(*prep.seat, again);
    expect_equal(
        static_cast<long long>(prep.fake.roll),
        static_cast<long long>(roll_seeded),
        "a later start draws nothing"
    );
    check(
        oa::sim::ai::computer_players_reload_profile(&prep.players.state, prep.host, text), "reload"
    );
    shot = read_snapshot(*prep.seat, request);
    knowledge = oa::sim::ai::computer_player_knowledge(&prep.players.state, 1);
    compare_weights(shot, *knowledge, prep.players.state.type_count);
    compare_knowledge(shot, *knowledge, prep.players.state.type_count);
    expect_equal(shot.weight[ARMPW], 100, "weights reset");
    expect_equal(shot.weight_locked[ARMPW], 0, "weight locks reset");
    expect_equal(shot.limits[ARMLLT], -1, "limits reset");
    expect_equal(shot.limit_locked[ARMLLT], 0, "limit locks reset");
    expect_equal(shot.weight[ARMSOLAR], 50, "the leading line applies from the kept plan flag");
    expect_equal(shot.limits[ARMMEX], 4, "easy plan limit again");
    expect_equal(shot.plan, 1, "the plan flag is kept");
    expect_equal(shot.land[0], land_step, "the land grid is kept");
    expect_equal(shot.water[3], water_phase, "the water grid is kept");
    expect_equal(shot.grids_ready, 1, "a later start does not reseed");
    check(
        prep.players.state.players[1].present && !prep.players.state.players[0].present,
        "controllers kept"
    );
}

void test_profile_leading_lines(const PackageBytes& package) {
    const char* text = "// every difficulty\n"
                       "weight ARM 0.2\n"
                       "limit ARM 0\n"
                       "weight ARMLAB 4\n"
                       "limit ARMLAB 2\n"
                       "plan easy\n"
                       "limit ARMMEX 6\n"
                       "plan hard\n"
                       "limit ARMMEX 9\n";
    Prepared prep;
    prep.fake.player(0, OA_PLAYER_STATUS_LOCAL, 1, 5);
    prep.fake.player(1, OA_PLAYER_STATUS_COMPUTER, 10, 30);
    prep.fake.player(2, OA_PLAYER_STATUS_COMPUTER, 31, 40);
    describe_catalog(prep.players.state);
    oa::sim::ai::computer_players_set_difficulty(&prep.players.state, 2, OA_DIFFICULTY_HARD);
    copy_catalog(prep.fake, prep.players.state, prep.definitions);
    check(
        oa::sim::ai::computer_players_configure(
            &prep.players.state,
            text,
            std::span<const uint16_t>(kBuildLists, std::size(kBuildLists))
        ),
        "leading: configure"
    );
    prep.host = host_for(prep.fake);
    prep.facts.difficulty = OA_DIFFICULTY_EASY;
    prep.facts.ai_profile.path = "ai/profile.txt";
    prep.facts.ai_profile.text = text;
    check(
        oa::sim::ai::computer_players_initialize(&prep.players.state, prep.host),
        "leading: initialize"
    );
    ClassicBind bind;
    bind.use_player = true;
    bind.player = 1;
    bind.use_difficulty = true;
    bind.difficulty = OA_DIFFICULTY_EASY;
    bind.build_runs = kBuildLists;
    bind.build_run_length = static_cast<uint32_t>(std::size(kBuildLists));
    bind.build_kept = 31;
    bind.knowledge_fake = &prep.fake;
    bind.knowledge_players = &prep.players.state;
    seat_classic(prep, package, bind, nullptr, false);
    compare_seat_to_player(*prep.seat, prep, 1);
    Snapshot request;
    Snapshot shot = read_snapshot(*prep.seat, request);
    expect_equal(shot.weight[ARMPW], 20, "leading: the category weight");
    expect_equal(shot.limits[ARMPW], 0, "leading: the category limit");
    expect_equal(shot.weight[ARMLAB], 80, "leading: the unit weight");
    expect_equal(shot.limits[ARMLAB], 2, "leading: the unit limit");
    expect_equal(shot.limit_locked[ARMLAB], 1, "leading: the unit limit locks");
    expect_equal(shot.weight[CORMEX], 100, "leading: the other side is untouched");
    expect_equal(shot.limits[CORMEX], -1, "leading: the other side stays unlimited");
    expect_equal(shot.limits[ARMMEX], 6, "leading: the game's difficulty plan");
    fresh_at(*prep.seat, OA_DIFFICULTY_HARD);
    shot = read_snapshot(*prep.seat, request);
    compare_weights(shot, prep.players.state.players[2].knowledge, prep.players.state.type_count);
    expect_equal(shot.limits[ARMMEX], 9, "leading: the player's own plan");
    expect_equal(shot.weight[ARMPW], 20, "leading: the hard player keeps the category weight");
}

void test_difficulty_names(const PackageBytes& package) {
    using Names = oa::data::match_rules::AiDifficultyNamesNames;
    const char* text = "plan easy\nlimit ARMMEX 8\nplan hard\nlimit ARMMEX 1\n"
                       "plan medium\nlimit ARMSOLAR 4\n";
    oa::data::match_rules::AiDifficultyNames swapped{};
    swapped.enabled = true;
    swapped.names = {Names::hard, Names::medium, Names::easy};
    oa::data::match_rules::AiDifficultyNames baseline_on{};
    baseline_on.enabled = true;

    struct Case {
        const oa::data::match_rules::AiDifficultyNames* names;
        int32_t difficulty;
        int32_t mex_limit;
        int32_t solar_limit;
    };

    const Case cases[] = {
        {nullptr, OA_DIFFICULTY_EASY, 8, -1},
        {&baseline_on, OA_DIFFICULTY_EASY, 8, -1},
        {&swapped, 0, 1, -1},
        {&swapped, 1, -1, 4},
        {&swapped, 2, 8, -1},
    };
    for (const Case& one : cases) {
        Prepared prep;
        mark_players(prep.fake, 1);
        prepare(
            prep,
            package,
            text,
            kBuildLists,
            static_cast<uint32_t>(std::size(kBuildLists)),
            31,
            one.difficulty,
            one.difficulty,
            1,
            false,
            false,
            one.names
        );
        Snapshot request;
        const Snapshot shot = read_snapshot(*prep.seat, request);
        compare_weights(
            shot, prep.players.state.players[1].knowledge, prep.players.state.type_count
        );
        expect_equal(
            shot.limits[ARMMEX], one.mex_limit, "names: the plan the difficulty's keyword selects"
        );
        expect_equal(shot.limits[ARMSOLAR], one.solar_limit, "names: medium keeps its plan");
    }
}

void test_installed_data(const char* package_root) {
    const auto assets = oa::test::require_game_assets("the installed computer-player profile");
    const auto bytes = oa::test::read_game_file(assets, "ai/default.txt");
    check(!bytes.empty(), "the install holds ai/default.txt");
    const std::string text(bytes.begin(), bytes.end());
    PackageBytes package;
    package.load(package_root);
    Prepared prep;
    prep.fake.player(1, OA_PLAYER_STATUS_COMPUTER, 10, 30);
    prepare(
        prep,
        package,
        text.c_str(),
        kBuildLists,
        static_cast<uint32_t>(std::size(kBuildLists)),
        31,
        OA_DIFFICULTY_MEDIUM,
        OA_DIFFICULTY_MEDIUM,
        1,
        false,
        false,
        nullptr
    );
    Snapshot request;
    request.read_knowledge = true;
    const Snapshot shot = read_snapshot(*prep.seat, request);
    compare_weights(shot, prep.players.state.players[1].knowledge, prep.players.state.type_count);
    compare_knowledge(shot, prep.players.state.players[1].knowledge, prep.players.state.type_count);
    check(shot.limits[ARMMEX] >= 0, "the installed limit applied");
    check(shot.weight[ARMPW] < 100, "the installed ARM weight applied");
}

void test_profile_twins(const PackageBytes& package) {
    const char* catalog[] = {"plan",      "weight", "limit",  "any",      "easy",   "medium",
                             "hard",      "ARMCOM", "ARMMEX", "ARMSOLAR", "ARMLAB", "ARMPW",
                             "ARMCK",     "ARMFIG", "ARMPT",  "ARMLLT",   "CORMEX", "ARM",
                             "COMMANDER", "LEVEL1", "PLANT",  "KBOT",     "CONSTR", "VTOL",
                             "SHIP",      "CORE",   "arm",    "Level1",   "Plant",  "0.5",
                             ".5",        "1e2",    "0.5abc", "-3",       "inf",    "nan",
                             "0x10",      ""};
    constexpr int kWords = static_cast<int>(std::size(catalog));
    const char* scripts[] = {
        "weight VTOL 0.5\n",
        "weight LEVEL1 0.5\n",
        "weight ARM 0.2\nlimit ARMPW 3\n",
        "plan any\nweight SHIP 0.5\n",
        "weight ARMPT 0.5\n",
    };
    const uint16_t download_types[] = {ARMFIG, ARMCOM, ARMLLT, ARMPT};
    uint32_t rng = kProfileSeed;
    for (int profile_index = 0; profile_index < 300; ++profile_index) {
        std::string text;
        const int lines = 4 + static_cast<int>(next_bits(rng) % 6);
        for (int line = 0; line < lines; ++line) {
            const uint32_t kind = next_bits(rng) % 5;
            if (kind == 0) {
                text += "plan ";
                text += catalog[next_bits(rng) % 7];
            } else if (kind == 1 || kind == 2) {
                text += kind == 1 ? "weight " : "limit ";
                text += catalog[next_bits(rng) % kWords];
                text += " ";
                text += catalog[20 + (next_bits(rng) % 18)];
            } else if (kind == 3) {
                text += "weight ARM 0.2 # ";
                text += catalog[next_bits(rng) % kWords];
            } else if ((next_bits(rng) % 2) == 0) {
                text.append(140, 'A');
            } else {
                for (int word = 0; word < 24; ++word)
                    text += "word ";
            }
            text += (next_bits(rng) % 4) == 0 ? "\r\n" : "\n";
        }
        switch (profile_index % 16) {
        case 0:
            text += "weight ARM 0.5abc\n";
            break;
        case 1:
            text += "weight ARM .5\n";
            break;
        case 2:
            text += "weight ARM 1e2\n";
            break;
        case 3:
            text += "weight ARM 0x10\n";
            break;
        case 4:
            text += "weight ARM inf\n";
            break;
        case 5:
            text += "weight ARM nan\n";
            break;
        case 6:
            text += "weight ARM -3\n";
            break;
        case 7:
            text += "weight ARM \n";
            break;
        case 8:
            text += "limit ARMMEX 0.5\n";
            break;
        case 9:
            text += "# comment only\n";
            break;
        case 10:
            text += "plan any\r\nweight ALL 0.5\r\n";
            break;
        case 11:
            text += "// sample\n";
            break;
        case 12:
            text += "plan easy\nlimit ARMMEX 4\nplan hard\nlimit ARMMEX 9\n";
            break;
        case 13:
            text += "Weight PLANT 2\n";
            break;
        case 14:
            text += "limit level1 3\n";
            break;
        default:
            text += "plan medium\nweight ARMSOLAR 0.1\n";
            break;
        }
        const int computers = 1 + static_cast<int>(next_bits(rng) % 3);
        const int32_t game = static_cast<int32_t>(next_bits(rng) % 3);
        Prepared prep;
        mark_players(prep.fake, computers);
        describe_catalog(prep.players.state);
        if ((next_bits(rng) % 2) == 0) {
            const int count = 1 + static_cast<int>(next_bits(rng) % 2);
            for (int index = 0; index < count; ++index) {
                const uint16_t type = download_types[next_bits(rng) % std::size(download_types)];
                set_directive(
                    prep.players.state, type, scripts[next_bits(rng) % std::size(scripts)]
                );
            }
        }
        if (computers >= 2 && (next_bits(rng) % 2) == 0)
            oa::sim::ai::computer_players_set_difficulty(
                &prep.players.state, 2, static_cast<int32_t>((game + 1) % 3)
            );
        if (computers >= 3 && (next_bits(rng) % 2) == 0)
            oa::sim::ai::computer_players_set_difficulty(
                &prep.players.state, 3, static_cast<int32_t>((game + 2) % 3)
            );
        copy_catalog(prep.fake, prep.players.state, prep.definitions);
        check(
            oa::sim::ai::computer_players_configure(
                &prep.players.state,
                text,
                std::span<const uint16_t>(kBuildLists, std::size(kBuildLists))
            ),
            "twin configure"
        );
        prep.host = host_for(prep.fake);
        prep.host.difficulty = game;
        prep.facts.difficulty = game;
        prep.facts.ai_profile.path = "ai/profile.txt";
        prep.facts.ai_profile.text = text;
        check(
            oa::sim::ai::computer_players_initialize(&prep.players.state, prep.host),
            "twin initialize"
        );
        ClassicBind bind;
        bind.use_player = true;
        bind.player = 1;
        bind.use_difficulty = true;
        bind.difficulty = game;
        bind.build_runs = kBuildLists;
        bind.build_run_length = static_cast<uint32_t>(std::size(kBuildLists));
        bind.build_kept = 31;
        bind.knowledge_fake = &prep.fake;
        bind.knowledge_players = &prep.players.state;
        seat_classic(prep, package, bind, nullptr, false);
        Snapshot request;
        request.read_knowledge = true;
        const Snapshot started = read_snapshot(*prep.seat, request);
        const int32_t kept_plan = started.plan;
        for (uint8_t player = 1; player <= computers; ++player) {
            if (effective_difficulty(prep.players.state, player, game) == game)
                compare_seat_to_player(*prep.seat, prep, player);
        }
        for (uint8_t player = 1; player <= computers; ++player) {
            const int32_t noted = effective_difficulty(prep.players.state, player, game);
            if (noted == game)
                continue;
            fresh_at(*prep.seat, noted);
            Snapshot shot = read_snapshot(*prep.seat, request);
            compare_weights(
                shot, prep.players.state.players[player].knowledge, prep.players.state.type_count
            );
            compare_knowledge(
                shot, prep.players.state.players[player].knowledge, prep.players.state.type_count
            );
        }
        check(
            oa::sim::ai::computer_players_reload_profile(&prep.players.state, prep.host, text),
            "twin reload"
        );
        reload_at(*prep.seat, game, kept_plan);
        for (uint8_t player = 1; player <= computers; ++player) {
            if (effective_difficulty(prep.players.state, player, game) == game)
                compare_seat_to_player(*prep.seat, prep, player);
        }
        for (uint8_t player = 1; player <= computers; ++player) {
            const int32_t noted = effective_difficulty(prep.players.state, player, game);
            if (noted == game)
                continue;
            reload_at(*prep.seat, noted, kept_plan);
            Snapshot shot = read_snapshot(*prep.seat, request);
            compare_weights(
                shot, prep.players.state.players[player].knowledge, prep.players.state.type_count
            );
            compare_knowledge(
                shot, prep.players.state.players[player].knowledge, prep.players.state.type_count
            );
        }
    }
}

void test_knowledge_twins(const PackageBytes& package) {
    Prepared prep;
    prep.fake.units.assign(500, oa::Unit{});
    prep.fake.world.units = prep.fake.units.data();
    prep.fake.world.unit_slot_count = 500;
    for (std::size_t slot = 0; slot < prep.fake.units.size(); ++slot)
        prep.fake.units[slot].id = static_cast<uint16_t>(slot);
    prep.fake.player(1, OA_PLAYER_STATUS_COMPUTER, 0, 0);
    prep.fake.rolls.assign(64, 3);
    prepare(
        prep,
        package,
        "",
        kBuildLists,
        static_cast<uint32_t>(std::size(kBuildLists)),
        31,
        OA_DIFFICULTY_EASY,
        OA_DIFFICULTY_EASY,
        1,
        true,
        false,
        nullptr
    );
    auto* knowledge = oa::sim::ai::computer_player_knowledge(&prep.players.state, 1);
    check(knowledge != nullptr, "knowledge record");
    const uint16_t kinds[] = {ARMPW, ARMCOM, ARMSOLAR, ARMLAB};
    Snapshot request;
    request.read_weights = false;
    request.read_knowledge = true;
    uint32_t turn = 0;
    for (int scenario = 0; scenario < 200; ++scenario) {
        for (auto& unit : prep.fake.units)
            unit = {};
        for (std::size_t slot = 0; slot < prep.fake.units.size(); ++slot)
            prep.fake.units[slot].id = static_cast<uint16_t>(slot);
        oa::sim::ai::computer_knowledge_clear(&prep.players.state, *knowledge);
        KnowledgeTally empty;
        oa::sim::ai::computer_knowledge_settle(*knowledge, empty);
        prep.fake.knowledge_tick[1] = 0;
        uint32_t rng = kProfileSeed ^ (static_cast<uint32_t>(scenario) * 0x9E3779B9u);
        if (rng == 0)
            rng = 1;
        int live_of[4] = {};
        for (int step = 0; step < 120; ++step) {
            ++turn;
            const int action = static_cast<int>(next_bits(rng) % 5);
            const int kind = static_cast<int>(next_bits(rng) % 4);
            if (action == 0 && live_of[kind] < 120) {
                for (std::size_t slot = 1; slot < prep.fake.units.size(); ++slot) {
                    oa::Unit& unit = prep.fake.units[slot];
                    if ((unit.flags & OA_UNIT_FLAG_LIVE) != 0)
                        continue;
                    unit = {};
                    unit.id = static_cast<uint16_t>(slot);
                    unit.type_index = kinds[kind];
                    unit.owner_index = 1;
                    unit.flags = OA_UNIT_FLAG_LIVE;
                    unit.build_remaining = (next_bits(rng) % 4) == 0 ? 1.0F : 0.0F;
                    unit.position.x = static_cast<int32_t>(next_bits(rng));
                    unit.position.y = static_cast<int32_t>(next_bits(rng));
                    unit.position.z = static_cast<int32_t>(next_bits(rng));
                    ++live_of[kind];
                    break;
                }
            } else if (action == 1 || action == 2 || action == 3) {
                for (std::size_t slot = 1; slot < prep.fake.units.size(); ++slot) {
                    oa::Unit& unit = prep.fake.units[slot];
                    if ((unit.flags & OA_UNIT_FLAG_LIVE) == 0 || unit.type_index != kinds[kind])
                        continue;
                    if (action == 1) {
                        unit.position.x = static_cast<int32_t>(next_bits(rng));
                        unit.position.z = static_cast<int32_t>(next_bits(rng));
                    } else if (action == 2) {
                        unit.flags = unit.flags & ~OA_UNIT_FLAG_LIVE;
                        if (live_of[kind] > 0)
                            --live_of[kind];
                    } else {
                        unit.build_remaining = (next_bits(rng) % 2) == 0 ? 0.0F : 1.0F;
                    }
                    break;
                }
            }
            prep.fake.world.game.tick = turn;
            refresh_knowledge(prep.players.state, prep.fake, 1);
            play_classic(*prep.seat, turn);
            const Snapshot shot = read_snapshot(*prep.seat, request);
            compare_knowledge(shot, *knowledge, prep.players.state.type_count);
        }
    }
}

void test_grid_and_metal_twins(const PackageBytes& package) {
    for (int pattern = 0; pattern < 16; ++pattern) {
        Prepared prep;
        prep.fake.player(1, OA_PLAYER_STATUS_COMPUTER, 0, 0);
        std::vector<uint32_t> rolls(64);
        for (std::size_t index = 0; index < rolls.size(); ++index)
            rolls[index] = static_cast<uint32_t>(pattern * 19 + static_cast<int>(index) * 3 + 1);
        prep.fake.rolls = rolls;
        clear_metal();
        if (pattern != 0) {
            metal_cells[0][0] = true;
            metal_cells[1][3] = true;
            metal_cells[20][10] = true;
            metal_cells[63][0] = true;
            metal_cells[63][63] = true;
        }
        describe_catalog(prep.players.state);
        copy_catalog(prep.fake, prep.players.state, prep.definitions);
        check(
            oa::sim::ai::computer_players_configure(
                &prep.players.state,
                "",
                std::span<const uint16_t>(kBuildLists, std::size(kBuildLists))
            ),
            "grid configure"
        );
        prep.host = host_for(prep.fake);
        prep.host.metal_feature = test_metal_feature;
        prep.host.difficulty = OA_DIFFICULTY_EASY;
        prep.facts.difficulty = OA_DIFFICULTY_EASY;
        {
            RecordGuard recording(&prep.setup);
            const auto saved = prep.host.random;
            prep.host.random = capture_random;
            check(
                oa::sim::ai::computer_players_initialize(&prep.players.state, prep.host),
                "grid initialize"
            );
            prep.host.random = saved;
        }
        ClassicBind bind;
        bind.use_player = true;
        bind.player = 1;
        bind.use_difficulty = true;
        bind.difficulty = OA_DIFFICULTY_EASY;
        bind.build_runs = kBuildLists;
        bind.build_run_length = static_cast<uint32_t>(std::size(kBuildLists));
        bind.build_kept = 31;
        bind.knowledge_fake = &prep.fake;
        bind.knowledge_players = &prep.players.state;
        seat_classic(prep, package, bind, &prep.setup, false);
        Snapshot request;
        request.read_weights = false;
        request.read_grids = true;
        request.read_metal = true;
        Snapshot shot = read_snapshot(*prep.seat, request);
        expect_equal(shot.grids_ready, 0, "grids wait for the first turn");
        const auto roll_before = prep.fake.roll;
        play_classic(*prep.seat, 1);
        expect_equal(
            static_cast<long long>(prep.fake.roll - roll_before),
            2,
            "the eight grid seeds do not draw the shared stream"
        );
        shot = read_snapshot(*prep.seat, request);
        const auto* knowledge = oa::sim::ai::computer_player_knowledge(&prep.players.state, 1);
        check(knowledge != nullptr, "grid record");
        compare_grids(shot, *knowledge);
        compare_metal(shot, *knowledge);
        expect_equal(shot.grids_ready, 1, "grids are seeded once");
        if (pattern == 0)
            expect_equal(static_cast<long long>(shot.metal.size()), 0, "an empty metal map");
        else
            check(shot.metal.size() == 5, "the metal map lists every spot");
    }
}
