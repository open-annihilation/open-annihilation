// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The made-up brain map-reader: the match map, movement regions and their cost.

#include "match_fixture.hpp"

#include "internal.hpp"
#include "reach.hpp"

#include "oa/base/game_loop.hpp"
#include "oa/core/map_plot.h"
#include "oa/core/unit.h"
#include "oa/core/world.h"
#include "oa/data/brain_facts.hpp"
#include "oa/data/script_host/f32.hpp"
#include "oa/sim/ai.hpp"
#include "oa/sim/brains/check.hpp"
#include "oa/sim/brains/handles.hpp"
#include "oa/sim/brains/seat.hpp"
#include "oa/sim/ground_orders/movement_map.hpp"
#include "oa/sim/ground_orders/search_worker.hpp"
#include "oa/sim/state_hash.hpp"

#include "lua.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace {

namespace brains = oa::sim::brains;
namespace host = oa::data::script_host;
namespace orders = oa::sim::ground_orders;

constexpr int key_room = 32;
constexpr int text_room = 160;
constexpr int value_room = 80;
constexpr uint32_t determinism_ticks = 600;
constexpr int32_t wall_column = 32;
constexpr int32_t water_column = 50;
constexpr int32_t rough_cell = 20;
constexpr int32_t park_cell = 8;

struct Value {
    enum Kind : uint8_t { kind_absent, kind_flag, kind_whole, kind_words, kind_real };

    Kind kind{kind_absent};
    bool on{};
    lua_Integer whole{};
    float real{};
    char words[text_room]{};
};

struct Bag {
    char keys[value_room][key_room]{};
    Value values[value_room]{};
    int count{};
};

struct Probe {
    const char* source{};
};

struct PushRequest {
    brains::UnitRef ref{};
    brains::Seat* seat{};
    const char* global_name{};
};

struct WorkerView {
    bool exhausted{};
    uint32_t flags{};
    int32_t fan{};
    int32_t threshold{};
    int32_t budget{};
    std::array<int16_t, 2> start{};
    std::array<int16_t, 2> finish{};
    std::array<int32_t, 2> nearest{};
    uint32_t width{};
    uint32_t height{};
    std::size_t route{};
};

struct FlatSampler final : orders::MovementMapSampler {
    uint8_t classify_cell(int32_t column, int32_t) override { return column == 1 ? 0 : 3; }
};

int32_t world_of(int32_t cell) {
    return cell * 16 + 8;
}

int32_t fixed_of(int32_t whole) {
    return whole * 65536;
}

void copy_key(char* dest, const char* text, std::size_t length) {
    std::size_t used = length;
    if (used >= static_cast<std::size_t>(key_room))
        used = static_cast<std::size_t>(key_room - 1);
    if (text != nullptr)
        std::memcpy(dest, text, used);
    dest[used] = '\0';
}

const Value& find(const Bag& bag, const char* key) {
    for (int index = 0; index < bag.count; ++index) {
        if (std::strcmp(bag.keys[index], key) == 0)
            return bag.values[index];
    }
    static const Value missing;
    return missing;
}

void expect_int(const Bag& bag, const char* key, lua_Integer want) {
    const Value& got = find(bag, key);
    if (got.kind == Value::kind_whole && got.whole == want)
        return;
    throw std::runtime_error(
        std::string(key) + " got " + std::to_string(static_cast<long long>(got.whole)) + " want " +
        std::to_string(static_cast<long long>(want))
    );
}

void expect_flag(const Bag& bag, const char* key, bool want) {
    const Value& got = find(bag, key);
    if (got.kind == Value::kind_flag && got.on == want)
        return;
    throw std::runtime_error(std::string(key) + (got.on ? " was true" : " was false"));
}

void expect_text(const Bag& bag, const char* key, const char* part) {
    const Value& got = find(bag, key);
    if (got.kind == Value::kind_words && part != nullptr && std::strstr(got.words, part) != nullptr)
        return;
    throw std::runtime_error(
        std::string(key) + " got '" + got.words + "' want '" + (part == nullptr ? "" : part) + "'"
    );
}

void store_value(Bag& bag, lua_State* lua, const char* key, std::size_t length) {
    CHECK(bag.count < value_room);
    copy_key(bag.keys[bag.count], key, length);
    Value& value = bag.values[bag.count];
    ++bag.count;
    float number = 0;
    if (lua_type(lua, -1) == LUA_TNIL)
        return;
    if (lua_isboolean(lua, -1) != 0) {
        value.kind = Value::kind_flag;
        value.on = lua_toboolean(lua, -1) != 0;
        return;
    }
    if (lua_isinteger(lua, -1) != 0) {
        value.kind = Value::kind_whole;
        value.whole = lua_tointeger(lua, -1);
        return;
    }
    if (host::to_f32(lua, -1, &number)) {
        value.kind = Value::kind_real;
        value.real = number;
        return;
    }
    if (lua_type(lua, -1) == LUA_TSTRING) {
        std::size_t size = 0;
        const char* text = lua_tolstring(lua, -1, &size);
        value.kind = Value::kind_words;
        if (size >= static_cast<std::size_t>(text_room))
            size = static_cast<std::size_t>(text_room - 1);
        if (text != nullptr)
            std::memcpy(value.words, text, size);
        value.words[size] = '\0';
    }
}

void read_got(void* context, lua_State* lua) {
    auto* bag = static_cast<Bag*>(context);
    bag->count = 0;
    lua_getglobal(lua, "got");
    if (!lua_istable(lua, -1)) {
        lua_pop(lua, 1);
        return;
    }
    const int table = lua_gettop(lua);
    lua_pushnil(lua);
    while (lua_next(lua, table) != 0) {
        std::size_t length = 0;
        const char* key = lua_tolstring(lua, -2, &length);
        if (key != nullptr)
            store_value(*bag, lua, key, length);
        lua_pop(lua, 1);
    }
    lua_pop(lua, 1);
}

int push_chunk(void* context, lua_State* lua) {
    const auto* probe = static_cast<const Probe*>(context);
    if (probe == nullptr || probe->source == nullptr)
        return luaL_error(lua, "missing probe");
    if (luaL_loadstring(lua, probe->source) != 0) {
        const char* message = lua_tostring(lua, -1);
        if (message == nullptr)
            message = "probe failed to load";
        return luaL_error(lua, "%s", message);
    }
    return 1;
}

void run_chunk(host::Sandbox& sandbox, const char* source) {
    Probe probe{source};
    host::CallHooks hooks;
    hooks.context = &probe;
    hooks.push = push_chunk;
    host::ScriptError error;
    if (!sandbox.call("probe", hooks, error))
        throw std::runtime_error(error.message.empty() ? "probe failed" : error.message);
}

Bag snap(host::Sandbox& sandbox) {
    Bag bag;
    host::ScriptError error;
    if (!sandbox.run_host("read", &bag, read_got, error))
        throw std::runtime_error(error.message.empty() ? "read failed" : error.message);
    return bag;
}

Bag probe(host::Sandbox& sandbox, const char* source) {
    run_chunk(sandbox, source);
    return snap(sandbox);
}

uint64_t spent(host::Sandbox& sandbox, const char* source) {
    run_chunk(sandbox, source);
    return sandbox.instructions_used();
}

void push_named(void* context, lua_State* lua) {
    auto* request = static_cast<PushRequest*>(context);
    if (request == nullptr || request->seat == nullptr || request->global_name == nullptr)
        return;
    brains::push_unit(lua, *request->seat, request->ref);
    lua_setglobal(lua, request->global_name);
}

void push_global(
    host::Sandbox& sandbox, brains::Seat& seat, brains::UnitRef ref, const char* name
) {
    PushRequest request{ref, &seat, name};
    host::ScriptError error;
    if (!sandbox.run_host("push-unit", &request, push_named, error))
        throw std::runtime_error(error.message.empty() ? "push unit failed" : error.message);
}

std::string load_script() {
    const std::string path = std::string(OA_MAP_READER_DIR) + "/brain.lua";
    std::ifstream input(path);
    CHECK(input);
    std::string text;
    std::string line;
    while (std::getline(input, line)) {
        text.append(line);
        text.push_back('\n');
    }
    CHECK(!text.empty());
    return text;
}

brains::Seat* seat_for(Fixture& fixture) {
    const auto& held = fixture.match->seat_state();
    CHECK(static_cast<bool>(held));
    auto* seated = static_cast<brains::Brains*>(held.get());
    CHECK(seated != nullptr && seated->seats[computer] != nullptr);
    return seated->seats[computer].get();
}

void name_class(Fixture& fixture) {
    fixture.defs[vehicle].movement_class = "KBOT";
    fixture.match->state().unit_defs[vehicle].move_class = 1;
}

void shape_land(Fixture& fixture) {
    auto& spatial = fixture.match->spatial();
    const uint32_t width = spatial.terrain_width;
    CHECK(width == static_cast<uint32_t>(map_cells));
    for (uint32_t row = 0; row < spatial.terrain_height; ++row) {
        auto& wall = spatial.plots[row * width + static_cast<uint32_t>(wall_column)];
        wall.low_height = 40;
        wall.high_height = 80;
        auto& water = spatial.plots[row * width + static_cast<uint32_t>(water_column)];
        water.low_height = 0;
        water.high_height = 0;
    }
    for (int32_t row = 0; row < 2; ++row) {
        for (int32_t column = 0; column < 2; ++column) {
            auto& plot = spatial.plots
                             [static_cast<uint32_t>(rough_cell + row) * width +
                              static_cast<uint32_t>(rough_cell + column)];
            plot.low_height = 40;
            plot.high_height = 55;
        }
    }
    auto& sample = spatial.plots[2 * width + 1];
    sample.low_height = 11;
    sample.high_height = 19;
    fixture.match->refresh_movement_maps({0, 0}, {map_cells, map_cells});

    spatial.plots[0].metal = 4;
    spatial.plots[0].metal_feature = false;
    const int32_t metal_cells[][2] = {{3, 5}, {4, 5}, {3, 6}};
    for (const auto& cell : metal_cells) {
        auto& plot =
            spatial.plots[static_cast<uint32_t>(cell[1]) * width + static_cast<uint32_t>(cell[0])];
        plot.metal = 9;
        plot.metal_feature = true;
        plot.indestructible_feature = true;
        plot.feature_word = 1;
    }
    const int32_t geo_cells[][2] = {{6, 2}, {1, 7}};
    for (const auto& cell : geo_cells) {
        auto& plot =
            spatial.plots[static_cast<uint32_t>(cell[1]) * width + static_cast<uint32_t>(cell[0])];
        plot.geo_feature = true;
        plot.feature_word = 2;
    }
    oa::MapPlot* corner = oa::world_plot(&fixture.match->state(), 1, 2);
    CHECK(corner != nullptr);
    corner->height = 77;
    fixture.match->state().game.tidal_strength = 0.25F;
    fixture.match->wind().speed = 12;
    fixture.match->wind().direction = 90;
}

void open_gap(Fixture& fixture) {
    // The match hides the top rows, so the gap is below them. Two plot rows
    // open a footprint-2 anchor.
    auto& spatial = fixture.match->spatial();
    constexpr uint32_t gap_row = 8;
    for (uint32_t row = gap_row; row < gap_row + 2; ++row) {
        auto& plot =
            spatial.plots[row * spatial.terrain_width + static_cast<uint32_t>(wall_column)];
        plot.low_height = 40;
        plot.high_height = 40;
    }
    fixture.match->refresh_movement_maps(
        {static_cast<int16_t>(wall_column), static_cast<int16_t>(gap_row)}, {1, 2}
    );
}

host::Sandbox& seat_reader(Fixture& fixture, const std::string& script, bool aware) {
    const auto bytes =
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(script.data()), script.size()};
    brains::PackageFile file{"brain.lua", bytes};
    brains::SeatSpec spec;
    spec.player = computer;
    spec.id = "map-reader";
    spec.name = "Map Reader";
    spec.version = "1";
    spec.entry = "brain.lua";
    spec.level = "steady";
    spec.difficulty = OA_DIFFICULTY_MEDIUM;
    spec.full_awareness = aware;
    spec.files = std::span<const brains::PackageFile>{&file, 1};
    const brains::SeatOutcome outcome = brains::seat_brain(*fixture.match, spec);
    if (outcome.result != brains::SeatResult::seated) {
        throw std::runtime_error(
            std::string("seat failed: ") + outcome.reason.data() + " " + outcome.error.message
        );
    }
    host::Sandbox* sandbox = brains::seat_sandbox(*fixture.match, computer);
    CHECK(sandbox != nullptr);
    return *sandbox;
}

const char* surface_source = R"(
local map = game.map
local function text_of(fn)
    local ok, err = pcall(fn)
    if ok then
        return "no error"
    end
    return tostring(err)
end
local speed, direction = map:wind()
local low, high = map:heights(1, 2)
local spots = map:metal_spots()
local geos = map:geo_spots()
local classes = map:classes()
local ax, az = map:anchor("kbot", {x = 248, z = 248})
got = {
    name = map.name,
    name_empty = map.name == "",
    path = map.path,
    path_nil = map.path == nil,
    world_nil = map.world == nil,
    width = map.width,
    cells = map:height(),
    height_fn = type(map.height) == "function",
    width_num = type(map.width) == "number",
    world_width = map.world_width,
    world_height = map.world_height,
    sea = map.sea_level,
    wind_min = map.wind_min,
    wind_max = map.wind_max,
    surface = map.surface_metal,
    tide = map.tide,
    wind_speed = speed,
    wind_dir = direction,
    corner = map:height(1, 2),
    corner_nil = map:height(-1, 0) == nil,
    low = low,
    high = high,
    heights_nil = map:heights(70, 0) == nil,
    ground = map:ground(168, 168),
    ground_off = map:ground(-1, 0),
    metal = map:metal(3, 5),
    metal_off = map:metal(-1, 0),
    feature = map:metal_feature(3, 5),
    feature_off = map:metal_feature(64, 0),
    plain = map:metal_feature(0, 0),
    spot_n = #spots,
    s1x = spots[1].cx, s1z = spots[1].cz,
    s2x = spots[2].cx, s2z = spots[2].cz,
    s3x = spots[3].cx, s3z = spots[3].cz,
    geo_n = #geos,
    g1x = geos[1].cx, g1z = geos[1].cz,
    g2x = geos[2].cx, g2z = geos[2].cz,
    class_n = #classes,
    class_name = classes[1].name,
    class_handle = classes[1].handle,
    class_fx = classes[1].footprint_x,
    class_fz = classes[1].footprint_z,
    pass_open = map:passable("Kbot", 10, 10),
    pass_rough = map:passable(1, 20, 20),
    pass_wall = map:passable("KBOT", 32, 10),
    pass_water = map:passable("kbot", 50, 10),
    pass_off = map:passable("kbot", -1, 0),
    anchor_x = ax, anchor_z = az,
    reach_open = map:reachable({x = 168, z = 168}, {x = 200, z = 168}, "kbot"),
    reach_wall = map:reachable({x = 168, z = 168}, {x = 648, z = 168}, "KBOT"),
    reach_water = map:reachable({x = 648, z = 168}, {x = 936, z = 168}, 1),
    reach_off = map:reachable({x = -1000, z = 168}, {x = 168, z = 168}, "kbot"),
    reach_edge = map:reachable({x = -8, z = 168}, {x = 72, z = 168}, "kbot"),
    reach_air = map:reachable({x = -1000, z = -1000}, {x = 9000, z = 9000}, "ARMSOLAR"),
    reach_air_ud = map:reachable({x = 1, z = 1}, {x = 2, z = 2}, units.ARMSOLAR),
    reach_type = map:reachable({x = 1, z = 1}, {x = 9, z = 9}, 2),
    reach_own = map:reachable(friend, {x = 200, z = 168}, "kbot"),
    hidden_nil = map:reachable(enemy, {x = 168, z = 168}, "boat") == nil,
    meta = getmetatable(map) == false,
    missing = map.no_such_field == nil,
    err_boat = text_of(function() map:passable("boat", 0, 0) end),
    err_solar = text_of(function() map:passable(units.ARMSOLAR, 0, 0) end),
    err_unit = text_of(function() map:reachable({x = 1, z = 1}, {x = 2, z = 2}, friend) end),
    err_nil = text_of(function() map:passable(nil, 0, 0) end),
    err_write = text_of(function() map.width = 3 end),
    err_bad = text_of(function() map:reachable(1, 2, "kbot") end),
}
)";

const char* reach_again = R"(
local map = game.map
got = {
    reach_around = map:reachable({x = 168, z = 168}, {x = 648, z = 168}, "kbot"),
}
)";

const char* reach_parked = R"(
local map = game.map
got = {
    pass_park = map:passable("kbot", 8, 8),
    reach_park = map:reachable({x = 136, z = 136}, {x = 200, z = 136}, "kbot"),
    reach_park_unit = map:reachable(parked, {x = 200, z = 136}, 1),
}
)";

const char* reach_charge = R"(game.map:reachable({x = 168, z = 168}, {x = 200, z = 168}, "kbot")
)";

const char* spot_charge = R"(got = game.map:metal_spots()
)";

const char* name_charge = R"(got = game.map.name
)";

const char* class_charge = R"(got = game.map:classes()
)";

void read_surface(Fixture& fixture, host::Sandbox& sandbox) {
    const oa::Game& game = fixture.match->state().game;
    const Bag empty = probe(sandbox, surface_source);
    expect_text(empty, "name", "");
    CHECK(find(empty, "name").words[0] == '\0');
    expect_flag(empty, "name_empty", true);
    expect_flag(empty, "path_nil", true);

    oa::data::brain_facts::GameFacts facts;
    facts.map_name = "Dry Flats";
    facts.map_path = "Maps/DryFlats.TNT";
    brains::set_match_game_facts(*fixture.match, facts);
    const Bag named = probe(sandbox, surface_source);
    expect_text(named, "name", "Dry Flats");
    expect_text(named, "path", "Maps/DryFlats.TNT");
    expect_flag(named, "path_nil", false);
    expect_text(named, "name", "Dry Flats");

    facts.map_path.clear();
    brains::set_match_game_facts(*fixture.match, facts);
    // The second read used the previous got table. Read path on its own.
    const Bag cleared =
        probe(sandbox, "got = { path_nil = game.map.path == nil, name = game.map.name }\n");
    expect_flag(cleared, "path_nil", true);
    expect_text(cleared, "name", "Dry Flats");

    const Bag got = named;
    expect_flag(got, "world_nil", true);
    expect_flag(got, "height_fn", true);
    expect_flag(got, "width_num", true);
    expect_int(got, "width", game.map_width);
    expect_int(got, "cells", game.map_height);
    expect_int(got, "world_width", game.map_width_world);
    expect_int(got, "world_height", game.map_height_world);
    expect_int(got, "sea", game.sea_level);
    expect_int(got, "wind_min", fixture.match->environment_wind().minimum_strength);
    expect_int(got, "wind_max", fixture.match->environment_wind().maximum_strength);
    expect_int(got, "surface", 4);
    expect_int(got, "wind_speed", 12);
    expect_int(got, "wind_dir", 90);
    const Value& tide = find(got, "tide");
    CHECK(tide.kind == Value::kind_real);
    CHECK(tide.real == 0.25F);
    expect_int(got, "corner", 77);
    expect_flag(got, "corner_nil", true);
    expect_int(got, "low", 11);
    expect_int(got, "high", 19);
    expect_flag(got, "heights_nil", true);
    expect_int(
        got,
        "ground",
        fixture.match->map_height(
            static_cast<uint32_t>(fixed_of(168)), static_cast<uint32_t>(fixed_of(168))
        )
    );
    expect_int(
        got, "ground_off", fixture.match->map_height(static_cast<uint32_t>(fixed_of(-1)), 0)
    );
    expect_int(got, "metal", 9);
    expect_int(got, "metal_off", 0);
    expect_flag(got, "feature", true);
    expect_flag(got, "feature_off", false);
    expect_flag(got, "plain", false);
    expect_int(got, "spot_n", 3);
    expect_int(got, "s1x", 3);
    expect_int(got, "s1z", 5);
    expect_int(got, "s2x", 4);
    expect_int(got, "s2z", 5);
    expect_int(got, "s3x", 3);
    expect_int(got, "s3z", 6);
    expect_int(got, "geo_n", 2);
    expect_int(got, "g1x", 6);
    expect_int(got, "g1z", 2);
    expect_int(got, "g2x", 1);
    expect_int(got, "g2z", 7);
    expect_int(got, "class_n", 1);
    expect_text(got, "class_name", "KBOT");
    expect_int(got, "class_handle", 1);
    expect_int(got, "class_fx", 2);
    expect_int(got, "class_fz", 2);
    expect_int(got, "pass_open", 3);
    expect_int(got, "pass_rough", 1);
    expect_int(got, "pass_wall", 0);
    expect_int(got, "pass_water", 0);
    expect_int(got, "pass_off", 0);
    expect_int(got, "anchor_x", 15);
    expect_int(got, "anchor_z", 15);
    expect_flag(got, "reach_open", true);
    expect_flag(got, "reach_wall", false);
    expect_flag(got, "reach_water", false);
    expect_flag(got, "reach_off", false);
    expect_flag(got, "reach_edge", true);
    expect_flag(got, "reach_air", true);
    expect_flag(got, "reach_air_ud", true);
    expect_flag(got, "reach_type", true);
    expect_flag(got, "reach_own", true);
    expect_flag(got, "hidden_nil", true);
    expect_flag(got, "meta", true);
    expect_flag(got, "missing", true);
    expect_text(got, "err_boat", "unknown movement class: boat");
    expect_text(got, "err_solar", "unknown movement class: ARMSOLAR");
    expect_text(got, "err_unit", "unknown movement class: unit");
    expect_text(got, "err_nil", "unknown movement class: nil");
    expect_text(got, "err_write", "map is read-only");
    expect_text(got, "err_bad", "reachable expects a position or a unit");
}

void read_charges(Fixture& fixture, host::Sandbox& sandbox) {
    const orders::MovementMap* grid = fixture.match->movement_map(1);
    CHECK(grid != nullptr);
    const uint32_t before_tick = grid->projection_tick();
    const uint8_t before_cell = grid->cell(10, 10);
    const auto worker_before = fixture.match->path_search_worker();
    const WorkerView worker_was = {
        worker_before.exhausted(),
        worker_before.order_flags(),
        worker_before.fan(),
        worker_before.threshold(),
        worker_before.budget(),
        worker_before.start_cell(),
        worker_before.finish_cell(),
        worker_before.nearest_goal(),
        worker_before.width(),
        worker_before.height(),
        worker_before.route().size()
    };

    const uint64_t cells =
        static_cast<uint64_t>(grid->width()) * static_cast<uint64_t>(grid->height());
    const uint64_t build = (cells + 15u) / 16u;
    fixture.match->state().game.tick = 1;
    const uint64_t first = spent(sandbox, reach_charge);
    const uint64_t second = spent(sandbox, reach_charge);
    CHECK(first > second);
    CHECK(first - second == build);
    fixture.match->state().game.tick = 2;
    const uint64_t third = spent(sandbox, reach_charge);
    const uint64_t fourth = spent(sandbox, reach_charge);
    CHECK(third - fourth == build);

    const uint64_t spots = spent(sandbox, spot_charge);
    const uint64_t spots_again = spent(sandbox, spot_charge);
    const uint64_t name = spent(sandbox, name_charge);
    CHECK(spots == spots_again);
    CHECK(spots >= name);
    CHECK(spots - name >= cells);
    const uint64_t classes = spent(sandbox, class_charge);
    CHECK(classes >= static_cast<uint64_t>(type_count - 1));

    CHECK(grid->projection_tick() == before_tick);
    CHECK(grid->cell(10, 10) == before_cell);
    const auto& worker_after = fixture.match->path_search_worker();
    CHECK(worker_after.exhausted() == worker_was.exhausted);
    CHECK(worker_after.order_flags() == worker_was.flags);
    CHECK(worker_after.fan() == worker_was.fan);
    CHECK(worker_after.threshold() == worker_was.threshold);
    CHECK(worker_after.budget() == worker_was.budget);
    CHECK(worker_after.start_cell() == worker_was.start);
    CHECK(worker_after.finish_cell() == worker_was.finish);
    CHECK(worker_after.nearest_goal() == worker_was.nearest);
    CHECK(worker_after.width() == worker_was.width);
    CHECK(worker_after.height() == worker_was.height);
    CHECK(worker_after.route().size() == worker_was.route);
}

void park_unit(Fixture& fixture, host::Sandbox& sandbox, brains::Seat& seat) {
    auto& slot = fixture.spawn(computer, vehicle, park_cell, park_cell);
    CHECK(slot.unit_index != 0);
    auto& spatial = fixture.match->spatial();
    CHECK(slot.unit_index < spatial.units.size());
    spatial.units[slot.unit_index].object_present = true;
    spatial.units[slot.unit_index].object_tick = 0;
    const uint32_t width = spatial.terrain_width;
    // Spawn claims the whole footprint. One plot is the block, so the 3x3
    // around the anchor still has an open cell.
    for (int32_t row = 0; row < 2; ++row) {
        for (int32_t column = 0; column < 2; ++column) {
            spatial
                .plots
                    [static_cast<uint32_t>(park_cell + row) * width +
                     static_cast<uint32_t>(park_cell + column)]
                .ground = 0;
        }
    }
    spatial.plots[static_cast<uint32_t>(park_cell) * width + static_cast<uint32_t>(park_cell)]
        .ground = slot.unit_index;

    orders::MovementMap* grid = const_cast<orders::MovementMap*>(fixture.match->movement_map(1));
    CHECK(grid != nullptr);
    uint32_t active_tick = 50;
    orders::OccupancyChange parked{orders::occupancy_rectangle(slot.record), 0, true};
    grid->prepare_search(orders::OccupancyRectangle{}, active_tick, std::span(&parked, 1), 100);
    const uint32_t projected = grid->projection_tick();
    CHECK(projected == 70);
    CHECK(grid->cell(static_cast<uint32_t>(park_cell), static_cast<uint32_t>(park_cell)) == 0);

    push_global(sandbox, seat, brains::unit_ref(*fixture.match, slot.unit_index), "parked");
    fixture.match->state().game.tick = 3;
    const Bag got = probe(sandbox, reach_parked);
    expect_int(got, "pass_park", 0);
    expect_flag(got, "reach_park", true);
    expect_flag(got, "reach_park_unit", true);
    CHECK(grid->projection_tick() == projected);
    CHECK(grid->cell(static_cast<uint32_t>(park_cell), static_cast<uint32_t>(park_cell)) == 0);
    CHECK(
        spatial.plots[static_cast<uint32_t>(park_cell) * width + static_cast<uint32_t>(park_cell)]
            .ground == slot.unit_index
    );
}

void sight_wall(const std::string& script) {
    Fixture fixture(0, true, {}, 1, 3);
    name_class(fixture);
    shape_land(fixture);
    host::Sandbox& sandbox = seat_reader(fixture, script, false);
    const char* across = R"(
got = {
    reach_wall = game.map:reachable({x = 168, z = 168}, {x = 648, z = 168}, "kbot"),
    reach_open = game.map:reachable({x = 168, z = 168}, {x = 200, z = 168}, "kbot"),
}
)";
    const Bag open = probe(sandbox, across);
    expect_flag(open, "reach_wall", true);
    expect_flag(open, "reach_open", true);

    auto& bits = fixture.match->sight_mutable().player_bits;
    CHECK(bits.size() == 32u * 32u);
    const auto mark = static_cast<uint16_t>(1u << computer);
    for (uint16_t& word : bits)
        word = mark;
    const Bag cached = probe(sandbox, across);
    expect_flag(cached, "reach_wall", true);

    fixture.match->state().game.tick = 1;
    const Bag mapped = probe(sandbox, across);
    expect_flag(mapped, "reach_wall", false);
    expect_flag(mapped, "reach_open", true);
}

void cache_keeps_four() {
    FlatSampler sampler;
    orders::MovementMap small(4, 4, 1, 1, sampler);
    orders::MovementMap wide(8, 8, 1, 1, sampler);
    small.rebuild();
    wide.rebuild();

    brains::RegionCache cache;
    brains::RegionBuild build;
    build.map = &small;
    build.handle = 1;
    build.tick = 1;
    CHECK(brains::ensure_regions(cache, build) == 1);
    CHECK(brains::region_queue_cells(cache) == 16);
    CHECK(brains::ensure_regions(cache, build) == 0);
    build.map = &wide;
    build.handle = 2;
    CHECK(brains::ensure_regions(cache, build) == 4);
    CHECK(brains::region_queue_cells(cache) == 64);

    brains::RegionCache labels;
    build.map = &small;
    build.handle = 7;
    build.tick = 3;
    CHECK(brains::ensure_regions(labels, build) == 1);
    CHECK(brains::region_label(labels, 7, 0, 0) == 1);
    CHECK(brains::region_label(labels, 7, 1, 0) == 0);
    CHECK(brains::region_label(labels, 7, 2, 0) == 2);
    CHECK(brains::regions_meet(labels, 7, 0, 0, 0, 3));
    CHECK(!brains::regions_meet(labels, 7, 0, 0, 3, 0));
    build.tick = 4;
    CHECK(brains::ensure_regions(labels, build) == 1);
    CHECK(brains::regions_for(labels, 7)->tick == 4);

    brains::RegionCache slots;
    orders::MovementMap maps[5] = {
        orders::MovementMap(4, 4, 1, 1, sampler),
        orders::MovementMap(4, 4, 1, 1, sampler),
        orders::MovementMap(4, 4, 1, 1, sampler),
        orders::MovementMap(4, 4, 1, 1, sampler),
        orders::MovementMap(4, 4, 1, 1, sampler)
    };
    const uint32_t handles[] = {9, 5, 4, 3, 1};
    const uint32_t ticks[] = {1, 2, 2, 2, 2};
    for (int index = 0; index < 5; ++index) {
        maps[index].rebuild();
        build.map = &maps[index];
        build.handle = handles[index];
        build.tick = ticks[index];
        CHECK(brains::ensure_regions(slots, build) == 1);
    }
    CHECK(brains::regions_for(slots, 9) == nullptr);
    CHECK(brains::regions_for(slots, 5) != nullptr);
    CHECK(brains::regions_for(slots, 4) != nullptr);
    CHECK(brains::regions_for(slots, 3) != nullptr);
    CHECK(brains::regions_for(slots, 1) != nullptr);

    brains::RegionCache ties;
    const uint32_t same_tick[] = {5, 4, 3, 2, 1};
    for (uint32_t handle : same_tick) {
        build.map = &small;
        build.handle = handle;
        build.tick = 8;
        CHECK(brains::ensure_regions(ties, build) == 1);
    }
    CHECK(brains::regions_for(ties, 2) == nullptr);
    CHECK(brains::regions_for(ties, 5) != nullptr);
    CHECK(brains::regions_for(ties, 4) != nullptr);
    CHECK(brains::regions_for(ties, 3) != nullptr);
    CHECK(brains::regions_for(ties, 1) != nullptr);

    brains::RegionBuild blank;
    CHECK(brains::ensure_regions(ties, blank) == 0);
    int32_t anchor_x = 0;
    int32_t anchor_z = 0;
    brains::anchor_cell(fixed_of(world_of(8)), fixed_of(world_of(3)), 2, 2, &anchor_x, &anchor_z);
    CHECK(anchor_x == 8 && anchor_z == 3);
}

WorkerView worker_view(const orders::SearchWorker& worker) {
    return WorkerView{
        worker.exhausted(),
        worker.order_flags(),
        worker.fan(),
        worker.threshold(),
        worker.budget(),
        worker.start_cell(),
        worker.finish_cell(),
        worker.nearest_goal(),
        worker.width(),
        worker.height(),
        worker.route().size()
    };
}

bool same_worker(const WorkerView& left, const WorkerView& right) {
    return left.exhausted == right.exhausted && left.flags == right.flags &&
           left.fan == right.fan && left.threshold == right.threshold &&
           left.budget == right.budget && left.start == right.start &&
           left.finish == right.finish && left.nearest == right.nearest &&
           left.width == right.width && left.height == right.height && left.route == right.route;
}

void advance(Fixture& fixture) {
    ++fixture.match->state().game.tick;
    fixture.match->tick();
}

void same_stream(const std::string& asking_script) {
    const char* idle_script = R"(
local hooks = {}
function hooks.fits()
    return true
end
function hooks.start()
end
return hooks
)";
    Fixture idle(0, true, {}, 1, 3);
    Fixture asking(0, true, {}, 1, 3);
    Fixture none(0, true, {}, 1, 3);
    name_class(idle);
    name_class(asking);
    name_class(none);
    CHECK(sim::ai::configure_match_computer_players(*idle.match, profile, build_lists, false));
    CHECK(sim::ai::configure_match_computer_players(*asking.match, profile, build_lists, false));
    CHECK(sim::ai::configure_match_computer_players(*none.match, profile, build_lists, false));
    const std::string idle_text = idle_script;
    seat_reader(idle, idle_text, true);
    seat_reader(asking, asking_script, true);
    CHECK(idle.match->random_state() == asking.match->random_state());
    for (uint32_t step = 0; step < determinism_ticks; ++step) {
        advance(idle);
        advance(asking);
        advance(none);
        if (idle.match->random_state() != asking.match->random_state())
            throw std::runtime_error("random diverged at tick " + std::to_string(step + 1));
    }
    host::Sandbox* sandbox = brains::seat_sandbox(*asking.match, computer);
    CHECK(sandbox != nullptr && !sandbox->failed());
    const oa::base::game_loop::Timing timing{};
    const uint64_t idle_hash = oa::sim::trace::match_state_hash(*idle.match, timing, 0, 0, nullptr);
    const uint64_t asking_hash =
        oa::sim::trace::match_state_hash(*asking.match, timing, 0, 0, nullptr);
    CHECK(idle_hash == asking_hash);
    CHECK(same_worker(
        worker_view(idle.match->path_search_worker()),
        worker_view(asking.match->path_search_worker())
    ));
    CHECK(same_worker(
        worker_view(none.match->path_search_worker()),
        worker_view(asking.match->path_search_worker())
    ));
}

void outside_a_match(const std::string& script) {
    const auto bytes =
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(script.data()), script.size()};
    brains::PackageFile file{"brain.lua", bytes};
    brains::SeatSpec spec;
    spec.player = computer;
    spec.id = "map-reader";
    spec.name = "Map Reader";
    spec.version = "1";
    spec.entry = "brain.lua";
    spec.level = "steady";
    spec.difficulty = OA_DIFFICULTY_MEDIUM;
    spec.files = std::span<const brains::PackageFile>{&file, 1};
    const brains::SeatOutcome outcome =
        brains::check_brain(spec, oa::data::brain_facts::GameFacts{});
    if (outcome.result != brains::SeatResult::seated) {
        throw std::runtime_error(
            std::string("check failed: ") + outcome.reason.data() + " " + outcome.error.message
        );
    }
}

} // namespace

int main() {
    try {
        const std::string script = load_script();
        outside_a_match(script);
        cache_keeps_four();

        Fixture fixture(0, true, {}, 1, 3);
        name_class(fixture);
        shape_land(fixture);
        CHECK(fixture.match->movement_map(1) != nullptr);
        host::Sandbox& sandbox = seat_reader(fixture, script, true);
        brains::Seat* seat = seat_for(fixture);
        auto& ally = fixture.spawn(computer, vehicle, 14, 14);
        auto& enemy = fixture.spawn(0, vehicle, 18, 18);
        push_global(sandbox, *seat, brains::unit_ref(*fixture.match, ally.unit_index), "friend");
        push_global(sandbox, *seat, brains::unit_ref(*fixture.match, enemy.unit_index), "enemy");
        read_surface(fixture, sandbox);
        read_charges(fixture, sandbox);
        open_gap(fixture);
        fixture.match->state().game.tick = 4;
        const Bag around = probe(sandbox, reach_again);
        expect_flag(around, "reach_around", true);
        park_unit(fixture, sandbox, *seat);
        sight_wall(script);
        same_stream(script);
        int32_t anchor_x = 0;
        int32_t anchor_z = 0;
        brains::anchor_cell(fixed_of(248), fixed_of(248), 2, 2, &anchor_x, &anchor_z);
        CHECK(anchor_x == 15 && anchor_z == 15);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "sim-brains-map: %s\n", error.what());
        return 1;
    }
}
