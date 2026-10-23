// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// What a seat may see. fog-reader has no full awareness. full-sight does.
// The fixture runs with mapping and line of sight on.

#include "match_fixture.hpp"

#include "internal.hpp"

#include "oa/base/game_loop.hpp"
#include "oa/core/player.h"
#include "oa/core/unit.h"
#include "oa/core/unit_def.h"
#include "oa/core/world.h"
#include "oa/data/script_host/fixed.hpp"
#include "oa/data/script_host/script_host.hpp"
#include "oa/sim/brains/check.hpp"
#include "oa/sim/brains/handles.hpp"
#include "oa/sim/brains/seat.hpp"
#include "oa/sim/detection.hpp"
#include "oa/sim/simulation_state.hpp"
#include "oa/sim/state_hash.hpp"
#include "oa/sim/visibility_state.hpp"

#include "lua.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace brains = oa::sim::brains;
namespace host = oa::data::script_host;

constexpr uint8_t ally_player = 2;
constexpr uint16_t mapping_and_line_of_sight = static_cast<uint16_t>(
    sim::visibility_state::terrain_mapping | sim::visibility_state::update_sight_grid
);
constexpr int32_t one_unit = static_cast<int32_t>(1u << 16);
constexpr int32_t radar_reach = 400;

struct Loaded {
    std::string script;
    std::string manifest;
    bool full{};
};

struct Chunk {
    const char* source{};
};

struct PushRequest {
    brains::Seat* seat{};
    brains::UnitRef ref{};
    const char* name{};
};

struct IntTable {
    const char* name{};
    std::vector<lua_Integer>* values{};
};

struct IntGlobal {
    const char* name{};
    lua_Integer value{};
    bool found{};
};

struct Held {
    bool present{};
    int has_x{};
    int64_t x{};
    int radar{};
    int sonar{};
    int current{};
    int has_type{};
    int radar_len{};
};

struct RawRow {
    bool found{};
    lua_Integer type_index{};
    bool x_ok{};
    bool y_ok{};
    bool z_ok{};
    int64_t x{};
    int64_t y{};
    int64_t z{};
};

struct LogRead {
    std::vector<std::string>* lines{};
};

std::string load_text(const std::string& path) {
    std::ifstream input(path);
    if (!input)
        throw std::runtime_error("missing " + path);
    std::string text;
    std::string line;
    while (std::getline(input, line)) {
        text.append(line);
        text.push_back('\n');
    }
    if (text.empty())
        throw std::runtime_error("empty " + path);
    return text;
}

Loaded load_package(const char* dir) {
    Loaded loaded;
    loaded.script = load_text(std::string(dir) + "/brain.lua");
    loaded.manifest = load_text(std::string(dir) + "/oabrain.yaml");
    loaded.full = loaded.manifest.find("fullAwareness: true") != std::string::npos;
    return loaded;
}

int32_t centre_fixed(int32_t cell) {
    return static_cast<int32_t>(static_cast<uint32_t>(cell * 16 + 8) << 16);
}

void advance(Fixture& fixture) {
    ++fixture.match->state().game.tick;
    fixture.match->tick();
}

void lift(Fixture& fixture, sim::unit_spawn::Slot& slot, int32_t cell_x, int32_t cell_z) {
    fixture.match->place_unit_at(
        slot.unit_index, centre_fixed(cell_x), one_unit, centre_fixed(cell_z), 1
    );
}

Fixture mapped_fixture() {
    return Fixture(0, true, {}, 0, 1, mapping_and_line_of_sight);
}

brains::Seat* seat_for(Fixture& fixture, uint8_t which) {
    const auto& held = fixture.match->seat_state();
    CHECK(static_cast<bool>(held));
    auto* seated = static_cast<brains::Brains*>(held.get());
    CHECK(seated != nullptr);
    CHECK(seated->seats[which] != nullptr);
    return seated->seats[which].get();
}

void require_seated(const brains::SeatOutcome& outcome) {
    if (outcome.result == brains::SeatResult::seated)
        return;
    throw std::runtime_error(
        std::string("seat failed: ") + outcome.reason.data() + " " + outcome.error.message
    );
}

brains::Seat* seat_files(
    Fixture& fixture,
    const Loaded& package,
    uint8_t player,
    const char* id,
    const char* name,
    bool full
) {
    const auto script_bytes = std::span<const uint8_t>{
        reinterpret_cast<const uint8_t*>(package.script.data()), package.script.size()
    };
    const auto manifest_bytes = std::span<const uint8_t>{
        reinterpret_cast<const uint8_t*>(package.manifest.data()), package.manifest.size()
    };
    brains::PackageFile files[] = {
        {"brain.lua", script_bytes},
        {"oabrain.yaml", manifest_bytes},
    };
    brains::SeatSpec spec;
    spec.player = player;
    spec.id = id;
    spec.name = name;
    spec.version = "1.0";
    spec.entry = "brain.lua";
    spec.level = "steady";
    spec.difficulty = OA_DIFFICULTY_MEDIUM;
    spec.full_awareness = full;
    spec.files = files;
    require_seated(brains::seat_brain(*fixture.match, spec));
    brains::Seat* seat = seat_for(fixture, player);
    CHECK(seat->full_awareness == full);
    return seat;
}

void seat_text(
    Fixture& fixture, const std::string& script, uint8_t player, const char* id, bool full
) {
    const auto bytes =
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(script.data()), script.size()};
    brains::PackageFile files[] = {{"brain.lua", bytes}};
    brains::SeatSpec spec;
    spec.player = player;
    spec.id = id;
    spec.name = id;
    spec.version = "1.0";
    spec.entry = "brain.lua";
    spec.level = "steady";
    spec.difficulty = OA_DIFFICULTY_MEDIUM;
    spec.full_awareness = full;
    spec.files = files;
    require_seated(brains::seat_brain(*fixture.match, spec));
}

void check_package(const Loaded& package, const char* id, const char* name) {
    const auto script_bytes = std::span<const uint8_t>{
        reinterpret_cast<const uint8_t*>(package.script.data()), package.script.size()
    };
    const auto manifest_bytes = std::span<const uint8_t>{
        reinterpret_cast<const uint8_t*>(package.manifest.data()), package.manifest.size()
    };
    brains::PackageFile files[] = {
        {"brain.lua", script_bytes},
        {"oabrain.yaml", manifest_bytes},
    };
    brains::SeatSpec spec;
    spec.player = computer;
    spec.id = id;
    spec.name = name;
    spec.version = "1.0";
    spec.entry = "brain.lua";
    spec.level = "steady";
    spec.difficulty = OA_DIFFICULTY_MEDIUM;
    spec.full_awareness = package.full;
    spec.files = files;
    oa::data::brain_facts::GameFacts facts{};
    require_seated(brains::check_brain(spec, facts));
}

int push_chunk(void* context, lua_State* lua) {
    const auto* chunk = static_cast<const Chunk*>(context);
    if (chunk == nullptr || chunk->source == nullptr)
        return luaL_error(lua, "missing probe");
    if (luaL_loadstring(lua, chunk->source) != 0) {
        const char* message = lua_tostring(lua, -1);
        if (message == nullptr)
            message = "probe failed to load";
        return luaL_error(lua, "%s", message);
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

void push_named(void* context, lua_State* lua) {
    auto* request = static_cast<PushRequest*>(context);
    if (request == nullptr || request->seat == nullptr || request->name == nullptr)
        return;
    brains::push_unit(lua, *request->seat, request->ref);
    lua_setglobal(lua, request->name);
}

void push_global(
    host::Sandbox& sandbox, brains::Seat& seat, brains::UnitRef ref, const char* name
) {
    PushRequest request{&seat, ref, name};
    host::ScriptError error;
    if (!sandbox.run_host("push-unit", &request, push_named, error))
        throw std::runtime_error(error.message.empty() ? "push failed" : error.message);
}

void read_int_table(void* context, lua_State* lua) {
    auto* table = static_cast<IntTable*>(context);
    if (table == nullptr || table->values == nullptr || table->name == nullptr)
        return;
    lua_getglobal(lua, table->name);
    if (lua_istable(lua, -1) == 0) {
        lua_pop(lua, 1);
        return;
    }
    const int count = static_cast<int>(lua_rawlen(lua, -1));
    for (int row = 1; row <= count; ++row) {
        lua_rawgeti(lua, -1, row);
        table->values->push_back(lua_tointeger(lua, -1));
        lua_pop(lua, 1);
    }
    lua_pop(lua, 1);
}

std::vector<lua_Integer> read_ints(host::Sandbox& sandbox, const char* name) {
    std::vector<lua_Integer> values;
    IntTable table{name, &values};
    host::ScriptError error;
    if (!sandbox.run_host("read-ints", &table, read_int_table, error))
        throw std::runtime_error(error.message.empty() ? "read failed" : error.message);
    return values;
}

void read_int_global(void* context, lua_State* lua) {
    auto* global = static_cast<IntGlobal*>(context);
    if (global == nullptr || global->name == nullptr)
        return;
    lua_getglobal(lua, global->name);
    if (lua_isinteger(lua, -1) != 0) {
        global->value = lua_tointeger(lua, -1);
        global->found = true;
    }
    lua_pop(lua, 1);
}

lua_Integer read_int(host::Sandbox& sandbox, const char* name) {
    IntGlobal global{name, 0, false};
    host::ScriptError error;
    if (!sandbox.run_host("read-int", &global, read_int_global, error))
        throw std::runtime_error(error.message.empty() ? "read failed" : error.message);
    if (!global.found)
        throw std::runtime_error(std::string(name) + " was not an integer");
    return global.value;
}

void expect_ints(
    const std::vector<lua_Integer>& got, const std::vector<lua_Integer>& want, const char* label
) {
    if (got.size() != want.size()) {
        throw std::runtime_error(
            std::string(label) + " length " + std::to_string(got.size()) + " want " +
            std::to_string(want.size())
        );
    }
    for (std::size_t index = 0; index < want.size(); ++index) {
        if (got[index] != want[index]) {
            throw std::runtime_error(
                std::string(label) + " at " + std::to_string(index) + " got " +
                std::to_string(got[index]) + " want " + std::to_string(want[index])
            );
        }
    }
}

void read_log(void* context, lua_State* lua) {
    auto* held = static_cast<LogRead*>(context);
    if (held == nullptr || held->lines == nullptr)
        return;
    lua_getglobal(lua, "log");
    if (lua_istable(lua, -1) == 0) {
        lua_pop(lua, 1);
        return;
    }
    const int count = static_cast<int>(lua_rawlen(lua, -1));
    for (int row = 1; row <= count; ++row) {
        lua_rawgeti(lua, -1, row);
        std::size_t length = 0;
        const char* text = lua_tolstring(lua, -1, &length);
        if (text != nullptr)
            held->lines->emplace_back(text, length);
        lua_pop(lua, 1);
    }
    lua_pop(lua, 1);
}

std::vector<std::string> read_lines(host::Sandbox& sandbox) {
    std::vector<std::string> lines;
    LogRead held{&lines};
    host::ScriptError error;
    if (!sandbox.run_host("read-log", &held, read_log, error))
        throw std::runtime_error(error.message.empty() ? "log failed" : error.message);
    return lines;
}

int count_name(const std::vector<std::string>& lines, const std::string& wanted) {
    int count = 0;
    for (const std::string& line : lines)
        count += line == wanted ? 1 : 0;
    return count;
}

int line_from(const std::vector<std::string>& lines, const std::string& wanted, int start) {
    for (int row = start; row < static_cast<int>(lines.size()); ++row) {
        if (lines[static_cast<std::size_t>(row)] == wanted)
            return row;
    }
    return -1;
}

void read_held(void* context, lua_State* lua) {
    auto* held = static_cast<Held*>(context);
    if (held == nullptr)
        return;
    lua_getglobal(lua, "held");
    if (lua_type(lua, -1) == LUA_TNIL) {
        lua_pop(lua, 1);
        return;
    }
    held->present = true;
    lua_getfield(lua, -1, "x");
    if (lua_type(lua, -1) == LUA_TNIL)
        held->has_x = 0;
    else if (host::to_fixed(lua, -1, &held->x))
        held->has_x = 1;
    lua_pop(lua, 1);
    auto flag = [&](const char* name, int* dest) {
        lua_getfield(lua, -1, name);
        if (lua_type(lua, -1) == LUA_TNIL)
            *dest = 2;
        else if (lua_isboolean(lua, -1) != 0)
            *dest = lua_toboolean(lua, -1);
        lua_pop(lua, 1);
    };
    flag("radar", &held->radar);
    flag("sonar", &held->sonar);
    flag("current", &held->current);
    lua_getfield(lua, -1, "type");
    held->has_type = lua_type(lua, -1) == LUA_TNIL ? 0 : 1;
    lua_pop(lua, 1);
    lua_pop(lua, 1);
    lua_getglobal(lua, "me");
    lua_getfield(lua, -1, "radar");
    if (lua_istable(lua, -1) != 0)
        held->radar_len = static_cast<int>(lua_rawlen(lua, -1));
    lua_pop(lua, 2);
}

Held held_contact(host::Sandbox& sandbox) {
    Held held;
    host::ScriptError error;
    if (!sandbox.run_host("read-held", &held, read_held, error))
        throw std::runtime_error(error.message.empty() ? "held failed" : error.message);
    return held;
}

void read_raw_row(void* context, lua_State* lua) {
    auto* row = static_cast<RawRow*>(context);
    if (row == nullptr)
        return;
    lua_getglobal(lua, "killed");
    if (lua_istable(lua, -1) == 0) {
        lua_pop(lua, 1);
        return;
    }
    row->found = true;
    lua_getfield(lua, -1, "type_index");
    row->type_index = lua_tointeger(lua, -1);
    lua_pop(lua, 1);
    auto component = [&](const char* name, int64_t* dest, bool* ok) {
        lua_getfield(lua, -1, name);
        *ok = host::to_fixed(lua, -1, dest);
        lua_pop(lua, 1);
    };
    component("x", &row->x, &row->x_ok);
    component("y", &row->y, &row->y_ok);
    component("z", &row->z, &row->z_ok);
    lua_pop(lua, 1);
}

int32_t cell_axis(int32_t fixed) {
    const auto word = static_cast<uint32_t>(fixed);
    const auto low = std::bit_cast<int16_t>(static_cast<uint16_t>(word >> 16));
    return static_cast<int32_t>(low) >> 5;
}

bool cell_on(const sim::visibility_state::PlayerSightGrid& grid, int32_t cx, int32_t cz) {
    return cx >= 0 && cz >= 0 && static_cast<uint32_t>(cx) < static_cast<uint32_t>(grid.width) &&
           static_cast<uint32_t>(cz) < static_cast<uint32_t>(grid.height);
}

bool sight_on(Fixture& fixture, int32_t cx, int32_t cz) {
    const auto& grid = fixture.match->sight();
    if (!cell_on(grid, cx, cz))
        return false;
    const auto coverage = fixture.match->player_coverage(computer);
    const std::size_t cell = static_cast<std::size_t>(cz) * static_cast<std::size_t>(grid.width) +
                             static_cast<std::size_t>(cx);
    if (cell >= coverage.size())
        return false;
    return sim::visibility_state::count_in_sight(
        coverage[cell], sim::visibility_state::SightReading::simulation
    );
}

bool explored_on(Fixture& fixture, int32_t cx, int32_t cz) {
    const auto& grid = fixture.match->sight();
    if (!cell_on(grid, cx, cz))
        return false;
    const std::size_t cell = static_cast<std::size_t>(cz) * static_cast<std::size_t>(grid.width) +
                             static_cast<std::size_t>(cx);
    if (cell >= grid.player_bits.size() || computer >= OA_PLAYER_COUNT)
        return false;
    const auto mask = static_cast<uint16_t>(uint16_t{1} << computer);
    return (grid.player_bits[cell] & mask) != 0;
}

std::vector<lua_Integer> occupied_slots(Fixture& fixture, int player_filter) {
    std::vector<lua_Integer> slots;
    const oa::World& world = fixture.match->state();
    for (uint32_t slot = 0; slot < world.unit_slot_count; ++slot) {
        if (slot > 65535u)
            continue;
        const oa::Unit& unit = world.units[slot];
        if (unit.type_index == 0)
            continue;
        if (player_filter >= 0 && unit.owner_index != static_cast<uint8_t>(player_filter))
            continue;
        slots.push_back(static_cast<lua_Integer>(slot));
    }
    return slots;
}

bool wait_refresh(Fixture& fixture) {
    for (uint32_t step = 0; step < 40; ++step) {
        const auto& sightings = fixture.match->sightings(computer);
        const uint32_t tick = fixture.match->state().game.tick;
        if (sightings.refreshed_tick > 0 && sightings.refreshed_tick == tick)
            return true;
        advance(fixture);
    }
    return false;
}

void open_outside(bool full, const char* id) {
    brains::Seat seat;
    seat.player = computer;
    seat.full_awareness = full;
    const host::Bank bank{
        host::default_tick_allowance, host::default_bank_ticks, host::default_call_time_limit
    };
    host::OpenOptions options;
    options.package_id = id;
    options.profile = brains::brain_profile(seat, bank);
    options.sources = seat.sources.hooks();
    options.random_seed = 0;
    host::ScriptError error;
    auto opened = host::Sandbox::open(options, error);
    if (!opened.has_value())
        throw std::runtime_error(error.message.empty() ? "open failed" : error.message);
    host::Sandbox sandbox(std::move(*opened));
    const brains::UnitRef ghost{1, 1};
    push_global(sandbox, seat, ghost, "ghost");
    if (!full) {
        run_chunk(
            sandbox,
            R"lua(
local function fail(message) error(message) end
if me.full_awareness ~= false then fail("awareness") end
if me.seen == nil or #me.seen ~= 0 then fail("seen") end
if me.seen_tick ~= nil then fail("seen tick") end
if me.radar == nil or #me.radar ~= 0 then fail("radar") end
if me.radar_tick ~= nil then fail("radar tick") end
if me:sees(0, 0) ~= false then fail("sees") end
if me:sees(1, 2, 3) ~= false then fail("sees y") end
if me:sees_unit(ghost) ~= false then fail("sees unit") end
if me.sight ~= nil or me.explored ~= nil then fail("grids") end
if game.units ~= nil then fail("units") end
if me.nearest_enemy ~= nil then fail("nearest") end
if me.seen_raw ~= nil then fail("raw") end
local function must_fail(fn, text)
  local ok, err = pcall(fn)
  if ok then error("expected an error containing " .. text) end
  if string.find(tostring(err), text, 1, true) == nil then
    error("error was " .. tostring(err) .. " missing " .. text)
  end
end
must_fail(function() me:sees("no", 0) end,
  "sees expects x and z as fixed values or whole numbers")
must_fail(function() me:sees(40000, 0) end,
  "sees expects x and z as fixed values or whole numbers")
must_fail(function() me:sees(0, 0, "no") end,
  "sees expects y as a fixed value or a whole number")
must_fail(function() me:sees_unit(1) end, "sees_unit expects a unit")
)lua"
        );
        return;
    }
    run_chunk(
        sandbox,
        R"lua(
local function fail(message) error(message) end
if me.full_awareness ~= true then fail("awareness") end
if me.seen == nil or #me.seen ~= 0 then fail("seen") end
if me:sees(4, 5) ~= false then fail("sees") end
if me:sees_unit(ghost) ~= false then fail("sees unit") end
if me.sight ~= nil then fail("sight") end
if game.units == nil then fail("units missing") end
local listed = game.units:list()
if #listed ~= 0 then fail("list") end
local walked = 0
game.units:each(function() walked = walked + 1 end)
if walked ~= 0 then fail("each") end
if me:nearest_enemy(3, 4) ~= nil then fail("nearest") end
local raw = me:seen_raw()
if raw == nil or #raw ~= 0 then fail("raw") end
local function must_fail(fn, text)
  local ok, err = pcall(fn)
  if ok then error("expected an error containing " .. text) end
  if string.find(tostring(err), text, 1, true) == nil then
    error("error was " .. tostring(err) .. " missing " .. text)
  end
end
must_fail(function() me:nearest_enemy("no", 0) end,
  "nearest_enemy expects x and z as fixed values or whole numbers")
must_fail(function() me:nearest_enemy(0, 0, 1) end,
  "nearest_enemy expects a filter table")
must_fail(function() game.units:list(10) end, "list expects a player from 0 to 9")
must_fail(function() game.units:list("no") end, "list expects a player from 0 to 9")
must_fail(function() game.units:each(1) end, "each expects a function")
must_fail(function() game.units.extra = 1 end, "units cannot be assigned")
)lua"
    );
}

void packages_and_outside(const Loaded& fog, const Loaded& full) {
    CHECK(!fog.full);
    CHECK(full.full);
    check_package(fog, "fog-reader", "Fog Reader");
    check_package(full, "full-sight", "Full Sight");
    open_outside(false, "fog-reader");
    open_outside(true, "full-sight");
}

const char* field_probe = R"lua(
local function fail(message) error(message) end
local function nil_field(unit, name)
  if unit[name] ~= nil then fail(name .. " should be hidden") end
end
local function has_field(unit, name)
  if unit[name] == nil then fail(name .. " should be visible") end
end
if hidden_now == 1 then
  if enemy.id == nil or enemy.slot ~= enemy_slot then fail("identity") end
  nil_field(enemy, "type")
  nil_field(enemy, "owner")
  nil_field(enemy, "x")
  nil_field(enemy, "health")
  nil_field(enemy, "health_percent")
  nil_field(enemy, "heading")
  nil_field(enemy, "finished")
  nil_field(enemy, "build_remaining")
else
  if enemy.slot ~= enemy_slot then fail("slot") end
  has_field(enemy, "type")
  has_field(enemy, "owner")
  has_field(enemy, "x")
  has_field(enemy, "heading")
  has_field(enemy, "finished")
  has_field(enemy, "build_remaining")
  if hide_health == 1 then
    nil_field(enemy, "health")
    nil_field(enemy, "health_percent")
  else
    if enemy.health ~= enemy_health then fail("health") end
    if enemy.health_percent == nil then fail("health percent") end
  end
  if enemy.owner ~= 0 then fail("owner") end
end
)lua";

void publish_field_flags(
    host::Sandbox& sandbox, uint16_t slot, int32_t health, bool hidden, bool hide
) {
    const std::string source = std::string("enemy_slot = ") + std::to_string(slot) +
                               "\nenemy_health = " + std::to_string(health) +
                               "\nhidden_now = " + (hidden ? "1" : "0") +
                               "\nhide_health = " + (hide ? "1" : "0") + "\n";
    run_chunk(sandbox, source);
}

void fog_reader_case(const Loaded& fog) {
    Fixture fixture = mapped_fixture();
    auto& watcher = fixture.spawn(computer, vehicle, 8, 8);
    auto& enemy = fixture.spawn(0, vehicle, 60, 8);
    auto& spare = fixture.spawn(0, vehicle, 50, 50);
    lift(fixture, watcher, 8, 8);
    lift(fixture, enemy, 60, 8);
    lift(fixture, spare, 50, 50);
    brains::Seat* seat = seat_files(fixture, fog, computer, "fog-reader", "Fog Reader", false);
    host::Sandbox* sandbox = brains::seat_sandbox(*fixture.match, computer);
    CHECK(sandbox != nullptr);
    push_global(*sandbox, *seat, brains::unit_ref(*fixture.match, watcher.unit_index), "watcher");
    push_global(*sandbox, *seat, brains::unit_ref(*fixture.match, enemy.unit_index), "enemy");
    advance(fixture);
    CHECK(!fixture.match->unit_visible(computer, enemy.unit_index));
    publish_field_flags(*sandbox, enemy.unit_index, enemy.record.health, true, false);
    run_chunk(*sandbox, field_probe);

    lift(fixture, enemy, 10, 8);
    bool seen = false;
    for (int step = 0; step < 5; ++step) {
        if (fixture.match->unit_visible(computer, enemy.unit_index)) {
            seen = true;
            break;
        }
        advance(fixture);
    }
    CHECK(seen);
    publish_field_flags(*sandbox, enemy.unit_index, enemy.record.health, false, false);
    run_chunk(*sandbox, field_probe);

    oa::UnitDef* def = oa::world_unit_def_of(&fixture.match->state(), &enemy.record);
    CHECK(def != nullptr);
    def->flags |= OA_UNIT_DEF_FLAG_HIDE_DAMAGE;
    publish_field_flags(*sandbox, enemy.unit_index, enemy.record.health, false, true);
    run_chunk(*sandbox, field_probe);
    def->flags &= ~OA_UNIT_DEF_FLAG_HIDE_DAMAGE;
    publish_field_flags(*sandbox, enemy.unit_index, enemy.record.health, false, false);
    run_chunk(*sandbox, field_probe);

    CHECK(wait_refresh(fixture));
    const auto& sightings = fixture.match->sightings(computer);
    CHECK(sightings.seen != nullptr);
    bool listed = false;
    for (uint32_t index = 0; index < sightings.seen_count; ++index)
        listed = listed || sightings.seen[index] == enemy.unit_index;
    CHECK(listed);
    const std::string seen_probe = std::string(
        "seen_slots = {}\nfor i, unit in ipairs(me.seen) do\n"
        "  seen_slots[i] = unit.slot\nend\nseen_tick_now = me.seen_tick\n"
    );
    run_chunk(*sandbox, seen_probe);
    std::vector<lua_Integer> want;
    want.reserve(sightings.seen_count);
    for (uint32_t index = 0; index < sightings.seen_count; ++index)
        want.push_back(static_cast<lua_Integer>(sightings.seen[index]));
    expect_ints(read_ints(*sandbox, "seen_slots"), want, "seen");
    CHECK(
        read_int(*sandbox, "seen_tick_now") == static_cast<lua_Integer>(sightings.refreshed_tick)
    );

    run_chunk(
        *sandbox,
        R"lua(
sees_row = {}
for _, y in ipairs({0, 64}) do
  for z = 0, 1024, 64 do
    for x = 0, 1024, 64 do
      sees_row[#sees_row + 1] = me:sees(x, z, y) and 1 or 0
    end
  end
end
sees_fixed = me:sees(watcher.x, watcher.z) and 1 or 0
local function must_fail(fn, text)
  local ok, err = pcall(fn)
  if ok then error("expected an error containing " .. text) end
  if string.find(tostring(err), text, 1, true) == nil then
    error("error was " .. tostring(err) .. " missing " .. text)
  end
end
must_fail(function() me:sees("no", 1) end,
  "sees expects x and z as fixed values or whole numbers")
must_fail(function() me:sees(0, 0, false) end,
  "sees expects y as a fixed value or a whole number")
)lua"
    );
    std::vector<lua_Integer> sees_want;
    for (const int y : {0, 64}) {
        for (int z = 0; z <= 1024; z += 64) {
            for (int x = 0; x <= 1024; x += 64) {
                const std::array<uint32_t, 3> point = {
                    static_cast<uint32_t>(static_cast<int32_t>(x) * 65536),
                    static_cast<uint32_t>(static_cast<int32_t>(y) * 65536),
                    static_cast<uint32_t>(static_cast<int32_t>(z) * 65536),
                };
                const bool on = fixture.match->point_visible(computer, point);
                sees_want.push_back(on ? 1 : 0);
            }
        }
    }
    expect_ints(read_ints(*sandbox, "sees_row"), sees_want, "sees");
    const oa::Unit& watcher_unit = fixture.match->state().units[watcher.unit_index];
    const std::array<uint32_t, 3> watcher_point = {
        static_cast<uint32_t>(watcher_unit.position.x),
        0,
        static_cast<uint32_t>(watcher_unit.position.z),
    };
    const lua_Integer fixed_want = fixture.match->point_visible(computer, watcher_point) ? 1 : 0;
    CHECK(read_int(*sandbox, "sees_fixed") == fixed_want);

    const brains::UnitRef spare_ref = brains::unit_ref(*fixture.match, spare.unit_index);
    push_global(*sandbox, *seat, spare_ref, "spare");
    fixture.match->kill_unit(
        spare.unit_index, static_cast<uint8_t>(sim::match_runtime::DeathKind::dismissed)
    );
    CHECK(!brains::unit_current(*fixture.match, spare_ref));
    run_chunk(
        *sandbox,
        R"lua(
sees_units = {
  me:sees_unit(watcher) and 1 or 0,
  me:sees_unit(enemy) and 1 or 0,
  me:sees_unit(spare) and 1 or 0,
}
local function must_fail(fn, text)
  local ok, err = pcall(fn)
  if ok then error("expected an error containing " .. text) end
  if string.find(tostring(err), text, 1, true) == nil then
    error("error was " .. tostring(err) .. " missing " .. text)
  end
end
must_fail(function() me:sees_unit(1) end, "sees_unit expects a unit")
)lua"
    );
    const std::vector<lua_Integer> unit_want = {
        fixture.match->unit_visible(computer, watcher.unit_index) ? 1 : 0,
        fixture.match->unit_visible(computer, enemy.unit_index) ? 1 : 0,
        0,
    };
    expect_ints(read_ints(*sandbox, "sees_units"), unit_want, "sees_unit");

    run_chunk(
        *sandbox,
        R"lua(
local function fail(message) error(message) end
local sight = me.sight
local explored = me.explored
if sight == nil or explored == nil then fail("grids") end
if sight.width ~= 32 or sight.height ~= 32 or sight.cell_size ~= 32 then fail("sight size") end
if explored.width ~= 32 or explored.height ~= 32 or explored.cell_size ~= 32 then
  fail("explored size")
end
sight_row = {}
explored_row = {}
for cz = 0, sight.height - 1 do
  for cx = 0, sight.width - 1 do
    sight_row[#sight_row + 1] = sight:at(cx, cz) and 1 or 0
    explored_row[#explored_row + 1] = explored:at(cx, cz) and 1 or 0
  end
end
if sight:at(-1, 0) then fail("off grid sight") end
if explored:at(0, -1) then fail("off grid explored") end
local function cell(x, z)
  local cx, cz = sight:cell_of(x, z)
  if cx == nil then return -999, -999 end
  return cx, cz
end
cell_row = {cell(0, 0)}
local cx, cz = cell(32, 64)
cell_row[#cell_row + 1] = cx
cell_row[#cell_row + 1] = cz
cx, cz = cell(-32, 0)
cell_row[#cell_row + 1] = cx
cell_row[#cell_row + 1] = cz
cx, cz = cell(2000, 0)
cell_row[#cell_row + 1] = cx
cell_row[#cell_row + 1] = cz
local function must_fail(fn, text)
  local ok, err = pcall(fn)
  if ok then error("expected an error containing " .. text) end
  if string.find(tostring(err), text, 1, true) == nil then
    error("error was " .. tostring(err) .. " missing " .. text)
  end
end
must_fail(function() sight:at(1.5, 0) end, "at expects cell coordinates")
must_fail(function() sight.at = 1 end, "a sight grid cannot be assigned")
if game.units ~= nil then fail("units") end
if me.nearest_enemy ~= nil then fail("nearest") end
if me.seen_raw ~= nil then fail("raw") end
)lua"
    );
    const auto& grid = fixture.match->sight();
    std::vector<lua_Integer> sight_want;
    std::vector<lua_Integer> explored_want;
    for (int32_t cz = 0; cz < grid.height; ++cz) {
        for (int32_t cx = 0; cx < grid.width; ++cx) {
            sight_want.push_back(sight_on(fixture, cx, cz) ? 1 : 0);
            explored_want.push_back(explored_on(fixture, cx, cz) ? 1 : 0);
        }
    }
    expect_ints(read_ints(*sandbox, "sight_row"), sight_want, "sight");
    expect_ints(read_ints(*sandbox, "explored_row"), explored_want, "explored");
    const int32_t samples[][2] = {{0, 0}, {32, 64}, {-32, 0}, {2000, 0}};
    std::vector<lua_Integer> cell_want;
    for (const auto& sample : samples) {
        const int32_t fixed_x = sample[0] * 65536;
        const int32_t fixed_z = sample[1] * 65536;
        const int32_t cx = cell_axis(fixed_x);
        const int32_t cz = cell_axis(fixed_z);
        if (!cell_on(grid, cx, cz)) {
            cell_want.push_back(-999);
            cell_want.push_back(-999);
        } else {
            cell_want.push_back(cx);
            cell_want.push_back(cz);
        }
    }
    expect_ints(read_ints(*sandbox, "cell_row"), cell_want, "cell_of");
}

void full_sight_case(const Loaded& full) {
    Fixture fixture = mapped_fixture();
    auto& watcher = fixture.spawn(computer, vehicle, 8, 8);
    auto& enemy = fixture.spawn(0, vehicle, 60, 8);
    lift(fixture, watcher, 8, 8);
    lift(fixture, enemy, 60, 8);
    brains::Seat* seat = seat_files(fixture, full, computer, "full-sight", "Full Sight", true);
    host::Sandbox* sandbox = brains::seat_sandbox(*fixture.match, computer);
    CHECK(sandbox != nullptr);
    push_global(*sandbox, *seat, brains::unit_ref(*fixture.match, enemy.unit_index), "enemy");
    advance(fixture);
    CHECK(!fixture.match->unit_visible(computer, enemy.unit_index));
    publish_field_flags(*sandbox, enemy.unit_index, enemy.record.health, false, false);
    run_chunk(*sandbox, field_probe);
    oa::UnitDef* def = oa::world_unit_def_of(&fixture.match->state(), &enemy.record);
    CHECK(def != nullptr);
    def->flags |= OA_UNIT_DEF_FLAG_HIDE_DAMAGE;
    publish_field_flags(*sandbox, enemy.unit_index, enemy.record.health, false, false);
    run_chunk(*sandbox, field_probe);
    def->flags &= ~OA_UNIT_DEF_FLAG_HIDE_DAMAGE;

    lift(fixture, enemy, 10, 8);
    bool seen = fixture.match->unit_visible(computer, enemy.unit_index);
    for (int step = 0; step < 5 && !seen; ++step) {
        advance(fixture);
        seen = fixture.match->unit_visible(computer, enemy.unit_index);
    }
    CHECK(seen);
    CHECK(wait_refresh(fixture));
    const auto& sightings = fixture.match->sightings(computer);
    bool listed = false;
    for (uint32_t index = 0; index < sightings.seen_count; ++index)
        listed = listed || sightings.seen[index] == enemy.unit_index;
    CHECK(listed);

    run_chunk(
        *sandbox,
        R"lua(
local function fail(message) error(message) end
local function slots_of(list)
  local rows = {}
  for i, unit in ipairs(list) do rows[i] = unit.slot end
  return rows
end
all_slots = slots_of(game.units:list())
own_slots = slots_of(game.units:list(1))
enemy_slots = slots_of(game.units:list(0))
local walked = {}
game.units:each(function(unit) walked[#walked + 1] = unit.slot end)
each_slots = walked
if #game.units:list(9) ~= 0 then fail("empty player") end
local function must_fail(fn, text)
  local ok, err = pcall(fn)
  if ok then error("expected an error containing " .. text) end
  if string.find(tostring(err), text, 1, true) == nil then
    error("error was " .. tostring(err) .. " missing " .. text)
  end
end
must_fail(function() game.units:list(10) end, "list expects a player from 0 to 9")
must_fail(function() game.units:list(-1) end, "list expects a player from 0 to 9")
must_fail(function() game.units:each("no") end, "each expects a function")
)lua"
    );
    expect_ints(read_ints(*sandbox, "all_slots"), occupied_slots(fixture, -1), "list");
    expect_ints(read_ints(*sandbox, "own_slots"), occupied_slots(fixture, computer), "list own");
    expect_ints(read_ints(*sandbox, "enemy_slots"), occupied_slots(fixture, 0), "list enemy");
    expect_ints(read_ints(*sandbox, "each_slots"), occupied_slots(fixture, -1), "each");

    run_chunk(
        *sandbox,
        R"lua(
nearest_row = {}
for i = 0, 49 do
  local x = (i * 37) % 1024
  local z = (i * 91) % 1024
  local queries = {
    function() return me:nearest_enemy(x, z) end,
    function() return me:nearest_enemy(x, z, {}) end,
    function() return me:nearest_enemy(x, z, {widened = true}) end,
    function() return me:nearest_enemy(x, z, {skip_submerged = true}) end,
    function() return me:nearest_enemy(x, z, {widened = true, skip_submerged = true}) end,
  }
  for _, query in ipairs(queries) do
    local unit = query()
    nearest_row[#nearest_row + 1] = unit and unit.slot or -1
  end
end
)lua"
    );
    uint8_t relation[OA_PLAYER_COUNT + 1]{};
    for (uint8_t player = 0; player < OA_PLAYER_COUNT; ++player) {
        relation[player] = fixture.match->allied(computer, player) ? static_cast<uint8_t>(1)
                                                                   : static_cast<uint8_t>(0);
    }
    relation[OA_PLAYER_COUNT] = static_cast<uint8_t>(1);
    sim::simulation_state::CandidateFilter filters[5]{};
    filters[2].widened_flags = true;
    filters[3].skip_submerged = true;
    filters[4].widened_flags = true;
    filters[4].skip_submerged = true;
    std::vector<lua_Integer> nearest_want;
    for (int index = 0; index < 50; ++index) {
        const int32_t x = ((index * 37) % 1024) * 65536;
        const int32_t z = ((index * 91) % 1024) * 65536;
        for (const auto& filter : filters) {
            oa::Unit* found = sim::simulation_state::nearest_candidate_unit(
                fixture.match->state(), relation, x, z, filter
            );
            if (found == nullptr) {
                nearest_want.push_back(-1);
                continue;
            }
            const uint32_t slot = oa::world_unit_slot(&fixture.match->state(), found);
            nearest_want.push_back(static_cast<lua_Integer>(slot));
        }
    }
    expect_ints(read_ints(*sandbox, "nearest_row"), nearest_want, "nearest");

    const oa::FixedVec3 stale = enemy.record.position;
    const uint16_t killed = enemy.unit_index;
    fixture.match->kill_unit(
        killed, static_cast<uint8_t>(sim::match_runtime::DeathKind::dismissed)
    );
    const auto& after = fixture.match->sightings(computer);
    bool still = false;
    for (uint32_t index = 0; index < after.seen_count; ++index)
        still = still || after.seen[index] == killed;
    CHECK(still);
    CHECK(fixture.match->state().units[killed].type_index == 0);
    run_chunk(
        *sandbox,
        std::string(
            "killed = nil\nfor _, row in ipairs(me:seen_raw()) do\n"
            "  if row.slot == "
        ) + std::to_string(killed) +
            " then killed = row end\nend\n"
    );
    RawRow row;
    host::ScriptError error;
    if (!sandbox->run_host("read-raw", &row, read_raw_row, error))
        throw std::runtime_error(error.message.empty() ? "raw failed" : error.message);
    CHECK(row.found);
    CHECK(row.type_index == 0);
    CHECK(row.x_ok && row.y_ok && row.z_ok);
    CHECK(row.x == stale.x);
    CHECK(row.y == stale.y);
    CHECK(row.z == stale.z);
}

void give_radar(Fixture& fixture, sim::unit_spawn::Slot& slot) {
    auto& table = fixture.match->state().unit_defs[vehicle];
    table.radar_distance = static_cast<int16_t>(radar_reach);
    table.sonar_distance = 0;
    oa::UnitDef* live = oa::world_unit_def_of(&fixture.match->state(), &slot.record);
    CHECK(live != nullptr);
    live->radar_distance = static_cast<int16_t>(radar_reach);
    live->sonar_distance = 0;
    slot.state_flags = static_cast<uint8_t>(slot.state_flags | OA_UNIT_STATE_ACTIVE);
}

void radar_case(const Loaded& fog) {
    Fixture fixture = mapped_fixture();
    fixture.match->simulation().players[ally_player].present = true;
    fixture.match->simulation().players[ally_player].status = OA_PLAYER_STATUS_LOCAL;
    std::array<uint8_t, 10> allies{};
    allies[computer] = 1;
    allies[ally_player] = 1;
    fixture.match->configure_player_alliances(computer, allies);
    CHECK(fixture.match->allied(computer, ally_player));
    CHECK(!fixture.match->allied(computer, 0));
    auto& tower = fixture.spawn(computer, vehicle, 8, 8);
    auto& enemy = fixture.spawn(0, vehicle, 32, 8);
    auto& ally = fixture.spawn(ally_player, vehicle, 12, 8);
    give_radar(fixture, tower);
    lift(fixture, tower, 8, 8);
    lift(fixture, enemy, 32, 8);
    lift(fixture, ally, 12, 8);
    (void)seat_files(fixture, fog, computer, "fog-reader", "Fog Reader", false);
    host::Sandbox* sandbox = brains::seat_sandbox(*fixture.match, computer);
    CHECK(sandbox != nullptr);
    run_chunk(*sandbox, "if me.radar_tick ~= 0 then error(\"radar tick\") end\n");
    advance(fixture);
    CHECK(fixture.match->player_contacts_tick(computer) == fixture.match->state().game.tick);
    const auto picture = fixture.match->player_contact_bits(computer);
    CHECK(enemy.unit_index < picture.size());
    CHECK((picture[enemy.unit_index] & sim::detection::picture_radar) != 0);
    CHECK((picture[enemy.unit_index] & sim::detection::picture_sonar) == 0);
    CHECK(!fixture.match->unit_visible(computer, enemy.unit_index));
    const int32_t near_x = enemy.record.position.x;
    fixture.match->apply_damage_event(tower, nullptr, static_cast<int16_t>(1), 1, 0);
    advance(fixture);
    const std::vector<std::string> after_contact = read_lines(*sandbox);
    const int damaged = line_from(after_contact, "damaged", 0);
    const int contact = line_from(after_contact, "contact", damaged + 1);
    const int later_tick = line_from(after_contact, "tick", contact + 1);
    if (damaged < 0 || contact < 0 || later_tick < 0)
        throw std::runtime_error("radar order");
    CHECK(count_name(after_contact, "contact") == 1);
    Held held = held_contact(*sandbox);
    CHECK(held.present);
    CHECK(held.has_x == 1);
    CHECK(held.x == near_x);
    CHECK(held.radar == 1);
    CHECK(held.sonar == 0);
    CHECK(held.current == 1);
    CHECK(held.has_type == 0);
    CHECK(held.radar_len == 1);

    lift(fixture, enemy, 62, 8);
    bool lost = false;
    for (int step = 0; step < 45; ++step) {
        advance(fixture);
        if (count_name(read_lines(*sandbox), "lost") == 1) {
            lost = true;
            break;
        }
    }
    CHECK(lost);
    const auto later = fixture.match->player_contact_bits(computer);
    CHECK(enemy.unit_index < later.size());
    CHECK(
        (later[enemy.unit_index] &
         (sim::detection::picture_radar | sim::detection::picture_sonar)) == 0
    );
    const std::vector<std::string> after_loss = read_lines(*sandbox);
    CHECK(count_name(after_loss, "contact") == 1);
    CHECK(count_name(after_loss, "lost") == 1);
    held = held_contact(*sandbox);
    CHECK(held.present);
    CHECK(held.has_x == 0);
    CHECK(held.current == 0);
    CHECK(held.radar == 1);
    CHECK(held.radar_len == 0);
}

void same_stream(Fixture& left, Fixture& right) {
    CHECK(left.match->random_state() == right.match->random_state());
    for (uint32_t step = 0; step < run_ticks; ++step) {
        advance(left);
        advance(right);
        if (left.match->random_state() != right.match->random_state())
            throw std::runtime_error("random diverged at tick " + std::to_string(step + 1));
    }
    const oa::base::game_loop::Timing timing{};
    const uint64_t left_hash = sim::trace::match_state_hash(*left.match, timing, 0, 0, nullptr);
    const uint64_t right_hash = sim::trace::match_state_hash(*right.match, timing, 0, 0, nullptr);
    CHECK(left_hash == right_hash);
}

void seat_pair(Fixture& fixture, const Loaded& fog, const Loaded& full, bool reading) {
    auto& tower = fixture.spawn(computer, vehicle, 8, 8);
    auto& enemy = fixture.spawn(0, vehicle, 32, 8);
    give_radar(fixture, tower);
    lift(fixture, tower, 8, 8);
    lift(fixture, enemy, 32, 8);
    fixture.match->simulation().players[ally_player].present = true;
    fixture.match->simulation().players[ally_player].status = OA_PLAYER_STATUS_COMPUTER;
    auto& resources = fixture.match->state().game.players[ally_player];
    resources.energy = resources.metal = starting_resources;
    if (reading) {
        (void)seat_files(fixture, fog, computer, "fog-reader", "Fog Reader", false);
        (void)seat_files(fixture, full, ally_player, "full-sight", "Full Sight", true);
        return;
    }
    const std::string quiet = "local hooks = {}\nfunction hooks.onTick() end\nreturn hooks\n";
    seat_text(fixture, quiet, computer, "fog-reader", false);
    seat_text(fixture, quiet, ally_player, "full-sight", true);
}

void determinism(const Loaded& fog, const Loaded& full) {
    // A seated brain replaces the built-in order tick, which draws from the
    // match random stream even when it has nothing to order. Two matches with
    // no brain, and two matches that both seat the same brains, stay paired.
    // The reading pair's turn reads every sight question; the quiet pair's
    // turn does not. me_test's two_seated_runs is the same pairing.
    Fixture quiet_left;
    Fixture quiet_right;
    same_stream(quiet_left, quiet_right);

    Fixture reading = mapped_fixture();
    Fixture quiet = mapped_fixture();
    seat_pair(reading, fog, full, true);
    seat_pair(quiet, fog, full, false);
    same_stream(reading, quiet);
}

} // namespace

int main() {
    try {
        const Loaded fog = load_package(OA_FOG_READER_DIR);
        const Loaded full = load_package(OA_FULL_SIGHT_DIR);
        packages_and_outside(fog, full);
        fog_reader_case(fog);
        full_sight_case(full);
        radar_case(fog);
        determinism(fog, full);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "sim-brains-sight: %s\n", ex.what());
        return 1;
    }
    return 0;
}
