// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The made-up brain side-reader: unit handles, the seat's side, squads,
// economy and the built-in player's knowledge snapshot.

#include "match_fixture.hpp"

#include "internal.hpp"

#include "oa/base/game_math.hpp"
#include "oa/core/player.h"
#include "oa/core/player_setup.h"
#include "oa/core/side.h"
#include "oa/core/unit.h"
#include "oa/core/unit_def.h"
#include "oa/core/weapon_def.h"
#include "oa/core/world.h"
#include "oa/data/mission_types.hpp"
#include "oa/sim/ai.hpp"
#include "oa/sim/brains/handles.hpp"
#include "oa/sim/state_hash.hpp"
#include "oa/sim/weapon_execution/retaliation.hpp"

#include "lua.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <span>
#include <string>
#include <string_view>

namespace {

namespace brains = oa::sim::brains;
namespace host = oa::data::script_host;

constexpr int number_room = 256;
constexpr int text_room = 64;
constexpr int double_room = 8;
constexpr int name_room = 32;
constexpr int text_bytes = 64;
constexpr uint8_t order_preserve_busy = 0x08;
constexpr uint8_t order_queue_idle = 0x40;
constexpr int32_t wide_component = 2000000000;
constexpr int32_t narrow_component = -2000000000;

struct Published {
    lua_Integer numbers[number_room]{};
    char names[number_room][name_room]{};
    int number_count{};
    char texts[text_room][text_bytes]{};
    char text_names[text_room][name_room]{};
    int text_count{};
    double doubles[double_room]{};
    char double_names[double_room][name_room]{};
    int double_count{};
};

struct Chunk {
    const char* source{};
};

struct PushRequest {
    brains::UnitRef ref{};
    brains::Seat* seat{};
    const char* global_name{};
};

struct Placed {
    uint16_t enemy{};
    uint16_t mover{};
    uint16_t collector{};
    uint16_t silo{};
    uint16_t second{};
    uint16_t quirk{};
    uint16_t spare{};
};

void copy_text(char* dest, std::size_t room, const char* text) {
    std::size_t used = 0;
    if (dest == nullptr || room == 0)
        return;
    if (text != nullptr) {
        while (text[used] != '\0' && used + 1 < room) {
            dest[used] = text[used];
            ++used;
        }
    }
    dest[used] = '\0';
}

void clear_published(Published& published) {
    published.number_count = 0;
    published.text_count = 0;
    published.double_count = 0;
}

void add_number(Published& published, const char* name, lua_Integer value) {
    CHECK(published.number_count < number_room);
    std::snprintf(published.names[published.number_count], name_room, "%s", name);
    published.numbers[published.number_count] = value;
    ++published.number_count;
}

void add_text(Published& published, const char* name, const char* text, std::size_t length) {
    CHECK(published.text_count < text_room);
    std::snprintf(published.text_names[published.text_count], name_room, "%s", name);
    std::size_t used = length;
    if (used > static_cast<std::size_t>(text_bytes - 1))
        used = static_cast<std::size_t>(text_bytes - 1);
    if (text != nullptr) {
        for (std::size_t byte = 0; byte < used; ++byte)
            published.texts[published.text_count][byte] = text[byte];
    }
    published.texts[published.text_count][used] = '\0';
    ++published.text_count;
}

void add_view(Published& published, const char* name, std::string_view text) {
    add_text(published, name, text.data(), text.size());
}

void add_double(Published& published, const char* name, double value) {
    CHECK(published.double_count < double_room);
    std::snprintf(published.double_names[published.double_count], name_room, "%s", name);
    published.doubles[published.double_count] = value;
    ++published.double_count;
}

void join_name(char* dest, std::size_t room, const char* prefix, const char* tail) {
    std::snprintf(dest, room, "%s%s", prefix, tail);
}

void publish_work(void* context, lua_State* lua) {
    auto* published = static_cast<Published*>(context);
    if (published == nullptr)
        return;
    for (int row = 0; row < published->number_count; ++row) {
        lua_pushinteger(lua, published->numbers[row]);
        lua_setglobal(lua, published->names[row]);
    }
    for (int row = 0; row < published->text_count; ++row) {
        lua_pushstring(lua, published->texts[row]);
        lua_setglobal(lua, published->text_names[row]);
    }
    for (int row = 0; row < published->double_count; ++row) {
        lua_pushnumber(lua, published->doubles[row]);
        lua_setglobal(lua, published->double_names[row]);
    }
}

void publish(host::Sandbox& sandbox, Published& published) {
    host::ScriptError error;
    if (!sandbox.run_host("publish", &published, publish_work, error))
        throw std::runtime_error(error.message.empty() ? "publish failed" : error.message);
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

lua_Integer bits_of(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    return static_cast<lua_Integer>(bits);
}

lua_Integer handle_id(brains::UnitRef ref) {
    return static_cast<lua_Integer>(ref.generation) * lua_Integer{65536} +
           static_cast<lua_Integer>(ref.slot);
}

const char* standing_move(uint32_t flags) {
    const uint32_t order = (flags & OA_UNIT_FLAG_MOVE_ORDER_MASK) >> OA_UNIT_FLAG_MOVE_ORDER_SHIFT;
    if (order == 0)
        return "hold";
    if (order == 1)
        return "manoeuvre";
    if (order == 2)
        return "roam";
    return nullptr;
}

const char* standing_fire(uint32_t flags) {
    const uint32_t order = (flags & OA_UNIT_FLAG_FIRE_ORDER_MASK) >> OA_UNIT_FLAG_FIRE_ORDER_SHIFT;
    if (order == 0)
        return "hold";
    if (order == 1)
        return "return";
    if (order == 2)
        return "at_will";
    return nullptr;
}

int32_t wrapped_difference(int32_t later, int32_t earlier) {
    return static_cast<int32_t>(static_cast<uint32_t>(later) - static_cast<uint32_t>(earlier));
}

uint32_t cell_at(int32_t cell) {
    return static_cast<uint32_t>(cell * 16 + 8) << 16;
}

std::string load_script() {
    const std::string path = std::string(OA_SIDE_READER_DIR) + "/brain.lua";
    std::ifstream input(path);
    if (!input)
        throw std::runtime_error("missing side-reader");
    std::string text;
    std::string line;
    while (std::getline(input, line)) {
        text.append(line);
        text.push_back('\n');
    }
    CHECK(!text.empty());
    return text;
}

brains::Seat* seat_for(Fixture& fixture, uint8_t which) {
    const auto& held = fixture.match->seat_state();
    CHECK(static_cast<bool>(held));
    auto* seated = static_cast<brains::Brains*>(held.get());
    CHECK(seated != nullptr);
    CHECK(seated->seats[which] != nullptr);
    return seated->seats[which].get();
}

void seat_reader(Fixture& fixture, const std::string& script, uint8_t which) {
    const auto bytes =
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(script.data()), script.size()};
    brains::PackageFile file{"brain.lua", bytes};
    brains::SeatSpec spec;
    spec.player = which;
    spec.id = "side-reader";
    spec.name = "Side Reader";
    spec.version = "1";
    spec.entry = "brain.lua";
    spec.level = "medium";
    spec.difficulty = OA_DIFFICULTY_MEDIUM;
    spec.files = std::span<const brains::PackageFile>{&file, 1};
    const brains::SeatOutcome outcome = brains::seat_brain(*fixture.match, spec);
    if (outcome.result != brains::SeatResult::seated) {
        throw std::runtime_error(
            std::string("seat failed: ") + outcome.reason.data() + " " + outcome.error.message
        );
    }
}

Placed place_units(Fixture& fixture) {
    Placed placed;
    placed.enemy = fixture.spawn(0, vehicle, 8, 8).unit_index;
    placed.mover = fixture.spawn(computer, vehicle, 40, 40).unit_index;
    placed.collector = fixture.spawn(computer, collector, 16, 40).unit_index;
    placed.silo = fixture.spawn(computer, silo, 40, 16).unit_index;
    placed.second = fixture.spawn(computer, vehicle, 52, 52).unit_index;
    placed.quirk = fixture.spawn(computer, relay, 16, 16).unit_index;
    placed.spare = fixture.spawn(computer, vehicle, 28, 28).unit_index;
    return placed;
}

void publish_unit(
    Published& published, const char* prefix, const oa::World& world, const oa::Unit& unit
) {
    char label[name_room];
    join_name(label, sizeof label, prefix, "_slot");
    add_number(published, label, unit.id);
    join_name(label, sizeof label, prefix, "_owner");
    add_number(published, label, unit.owner_index);
    join_name(label, sizeof label, prefix, "_type_index");
    add_number(published, label, unit.type_index);
    const oa::UnitDef* def = oa::world_unit_def_of(&world, &unit);
    CHECK(def != nullptr);
    join_name(label, sizeof label, prefix, "_type_name");
    add_view(published, label, std::string_view(def->unit_name, sizeof def->unit_name));
    join_name(label, sizeof label, prefix, "_x");
    add_number(published, label, unit.position.x);
    join_name(label, sizeof label, prefix, "_y");
    add_number(published, label, unit.position.y);
    join_name(label, sizeof label, prefix, "_z");
    add_number(published, label, unit.position.z);
    join_name(label, sizeof label, prefix, "_heading");
    add_number(published, label, unit.heading);
    join_name(label, sizeof label, prefix, "_health");
    add_number(published, label, unit.health);
    join_name(label, sizeof label, prefix, "_health_percent");
    add_number(published, label, unit.health_percent);
    join_name(label, sizeof label, prefix, "_max_health");
    add_number(published, label, static_cast<lua_Integer>(def->max_damage));
    join_name(label, sizeof label, prefix, "_remaining");
    add_number(published, label, bits_of(unit.build_remaining));
    const bool finished = !(unit.build_remaining < 0.0F) && !(unit.build_remaining > 0.0F);
    join_name(label, sizeof label, prefix, "_finished");
    add_number(published, label, finished ? 1 : 0);
    join_name(label, sizeof label, prefix, "_building");
    add_number(published, label, (unit.flags & OA_UNIT_FLAG_BUILDING) != 0 ? 1 : 0);
    join_name(label, sizeof label, prefix, "_armed");
    add_number(published, label, (unit.flags & OA_UNIT_FLAG_HAS_WEAPONS) != 0 ? 1 : 0);
    join_name(label, sizeof label, prefix, "_selectable");
    add_number(published, label, (unit.flags & OA_UNIT_FLAG_SELECTABLE) != 0 ? 1 : 0);
    join_name(label, sizeof label, prefix, "_live");
    add_number(published, label, oa::unit_is_live_target(unit.flags) ? 1 : 0);
    join_name(label, sizeof label, prefix, "_dying");
    add_number(published, label, (unit.flags & OA_UNIT_FLAG_DEATH_PENDING) != 0 ? 1 : 0);
    join_name(label, sizeof label, prefix, "_active");
    add_number(published, label, (unit.state_flags & OA_UNIT_STATE_ACTIVE) != 0 ? 1 : 0);
    join_name(label, sizeof label, prefix, "_cloaked");
    add_number(published, label, (unit.state_flags & OA_UNIT_STATE_CLOAKED) != 0 ? 1 : 0);
    join_name(label, sizeof label, prefix, "_mobile");
    add_number(published, label, unit.movement != 0 ? 1 : 0);
    join_name(label, sizeof label, prefix, "_squad");
    add_number(published, label, unit.squad);
    const char* move = standing_move(unit.flags);
    join_name(label, sizeof label, prefix, "_move_nil");
    add_number(published, label, move == nullptr ? 1 : 0);
    join_name(label, sizeof label, prefix, "_move");
    add_text(
        published, label, move == nullptr ? "" : move, move == nullptr ? 0 : std::strlen(move)
    );
    const char* fire = standing_fire(unit.flags);
    join_name(label, sizeof label, prefix, "_fire_nil");
    add_number(published, label, fire == nullptr ? 1 : 0);
    join_name(label, sizeof label, prefix, "_fire");
    add_text(
        published, label, fire == nullptr ? "" : fire, fire == nullptr ? 0 : std::strlen(fire)
    );
    join_name(label, sizeof label, prefix, "_kills");
    add_number(published, label, unit.veteran_level);
}

void publish_order(
    Published& published, const char* prefix, const sim::match_runtime::Match::OrderRecordView& row
) {
    char label[name_room];
    const auto names = oa::data::mission_types::registered_names();
    std::string_view kind;
    if (row.kind < names.size())
        kind = names[row.kind];
    join_name(label, sizeof label, prefix, "_kind");
    add_view(published, label, kind);
    join_name(label, sizeof label, prefix, "_target_nil");
    add_number(published, label, row.target == 0 ? 1 : 0);
    join_name(label, sizeof label, prefix, "_target");
    add_number(published, label, row.target);
    join_name(label, sizeof label, prefix, "_x");
    add_number(published, label, row.point[0]);
    join_name(label, sizeof label, prefix, "_y");
    add_number(published, label, row.point[1]);
    join_name(label, sizeof label, prefix, "_z");
    add_number(published, label, row.point[2]);
    join_name(label, sizeof label, prefix, "_p1");
    add_number(published, label, row.parameter_1);
    join_name(label, sizeof label, prefix, "_p2");
    add_number(published, label, row.parameter_2);
}

const char* preamble = R"lua(
local function expect(got, want, label)
    if got ~= want then
        error(label .. ": got " .. tostring(got) .. " want " .. tostring(want))
    end
end

local function flag(value, label)
    if value == true then
        return 1
    end
    if value == false then
        return 0
    end
    error(label .. " is " .. tostring(value))
end

local function must_fail(fn, text)
    local ok, err = pcall(fn)
    if ok then
        error("expected an error containing " .. text)
    end
    if string.find(tostring(err), text, 1, true) == nil then
        error("error was " .. tostring(err) .. " missing " .. text)
    end
end

local function by_slot(which)
    for _, unit in ipairs(me:units()) do
        if unit.slot == which then
            return unit
        end
    end
    error("no unit in slot " .. tostring(which))
end

local function raw_of(value, label)
    if value == nil or value.raw == nil then
        error(label .. " has no raw value")
    end
    return value:raw()
end

local function nil_field(unit, name)
    if unit[name] ~= nil then
        error(name .. " is " .. tostring(unit[name]))
    end
end

local own_fields = {
    "owner", "type_index", "type_name", "x", "y", "z", "position", "heading",
    "health", "health_percent", "max_health", "build_remaining", "finished",
    "building", "armed", "selectable", "live", "dying", "active", "cloaked",
    "mobile", "squad", "move_order", "fire_order", "kills",
}

local function nil_own(unit)
    for _, name in ipairs(own_fields) do
        nil_field(unit, name)
    end
    if unit:head_order() ~= nil then
        error("head_order")
    end
    if unit:has_background_order() ~= nil then
        error("has_background_order")
    end
    if unit:orders() ~= nil then
        error("orders")
    end
    if unit:reaches() ~= nil then
        error("reaches")
    end
end

local function seen_unit(unit, prefix)
    expect(unit.id, _G[prefix .. "_id"], prefix .. " id")
    expect(unit.slot, _G[prefix .. "_slot"], prefix .. " slot")
    expect(flag(unit.alive, prefix .. " alive"), 1, prefix .. " alive")
    expect(unit.owner, _G[prefix .. "_owner"], prefix .. " owner")
    expect(unit.type_index, _G[prefix .. "_type_index"], prefix .. " type")
    expect(unit.type_name, _G[prefix .. "_type_name"], prefix .. " type name")
    expect(raw_of(unit.x, prefix .. " x"), _G[prefix .. "_x"], prefix .. " x")
    expect(raw_of(unit.y, prefix .. " y"), _G[prefix .. "_y"], prefix .. " y")
    expect(raw_of(unit.z, prefix .. " z"), _G[prefix .. "_z"], prefix .. " z")
    expect(raw_of(unit.position.x, prefix .. " px"), _G[prefix .. "_x"], prefix .. " px")
    expect(raw_of(unit.position.y, prefix .. " py"), _G[prefix .. "_y"], prefix .. " py")
    expect(raw_of(unit.position.z, prefix .. " pz"), _G[prefix .. "_z"], prefix .. " pz")
    expect(unit.heading, _G[prefix .. "_heading"], prefix .. " heading")
    expect(unit.health, _G[prefix .. "_health"], prefix .. " health")
    expect(unit.health_percent, _G[prefix .. "_health_percent"], prefix .. " percent")
    expect(unit.max_health, _G[prefix .. "_max_health"], prefix .. " max health")
    expect(unit.build_remaining:bits(), _G[prefix .. "_remaining"], prefix .. " remaining")
    expect(flag(unit.finished, prefix .. " finished"), _G[prefix .. "_finished"], prefix .. " finished")
    expect(flag(unit.building, prefix .. " building"), _G[prefix .. "_building"], prefix .. " building")
end

local private_names = {
    "armed", "selectable", "live", "dying", "active", "cloaked",
    "mobile", "squad", "move_order", "fire_order", "kills",
}

local function nil_private(unit)
    for _, name in ipairs(private_names) do
        nil_field(unit, name)
    end
    if unit:head_order() ~= nil then
        error("head_order")
    end
    if unit:has_background_order() ~= nil then
        error("has_background_order")
    end
    if unit:orders() ~= nil then
        error("orders")
    end
    if unit:reaches() ~= nil then
        error("reaches")
    end
end

local function same_unit(unit, prefix)
    expect(unit.id, _G[prefix .. "_id"], prefix .. " id")
    expect(unit.slot, _G[prefix .. "_slot"], prefix .. " slot")
    expect(flag(unit.alive, prefix .. " alive"), 1, prefix .. " alive")
    expect(unit.owner, _G[prefix .. "_owner"], prefix .. " owner")
    expect(unit.type_index, _G[prefix .. "_type_index"], prefix .. " type")
    expect(unit.type_name, _G[prefix .. "_type_name"], prefix .. " type name")
    expect(raw_of(unit.x, prefix .. " x"), _G[prefix .. "_x"], prefix .. " x")
    expect(raw_of(unit.y, prefix .. " y"), _G[prefix .. "_y"], prefix .. " y")
    expect(raw_of(unit.z, prefix .. " z"), _G[prefix .. "_z"], prefix .. " z")
    expect(raw_of(unit.position.x, prefix .. " px"), _G[prefix .. "_x"], prefix .. " px")
    expect(raw_of(unit.position.y, prefix .. " py"), _G[prefix .. "_y"], prefix .. " py")
    expect(raw_of(unit.position.z, prefix .. " pz"), _G[prefix .. "_z"], prefix .. " pz")
    expect(unit.heading, _G[prefix .. "_heading"], prefix .. " heading")
    expect(unit.health, _G[prefix .. "_health"], prefix .. " health")
    expect(unit.health_percent, _G[prefix .. "_health_percent"], prefix .. " percent")
    expect(unit.max_health, _G[prefix .. "_max_health"], prefix .. " max health")
    expect(unit.build_remaining:bits(), _G[prefix .. "_remaining"], prefix .. " remaining")
    expect(flag(unit.finished, prefix .. " finished"), _G[prefix .. "_finished"], prefix .. " finished")
    expect(flag(unit.building, prefix .. " building"), _G[prefix .. "_building"], prefix .. " building")
    expect(flag(unit.armed, prefix .. " armed"), _G[prefix .. "_armed"], prefix .. " armed")
    expect(flag(unit.selectable, prefix .. " selectable"), _G[prefix .. "_selectable"], prefix .. " selectable")
    expect(flag(unit.live, prefix .. " live"), _G[prefix .. "_live"], prefix .. " live")
    expect(flag(unit.dying, prefix .. " dying"), _G[prefix .. "_dying"], prefix .. " dying")
    expect(flag(unit.active, prefix .. " active"), _G[prefix .. "_active"], prefix .. " active")
    expect(flag(unit.cloaked, prefix .. " cloaked"), _G[prefix .. "_cloaked"], prefix .. " cloaked")
    expect(flag(unit.mobile, prefix .. " mobile"), _G[prefix .. "_mobile"], prefix .. " mobile")
    expect(unit.squad, _G[prefix .. "_squad"], prefix .. " squad")
    if _G[prefix .. "_move_nil"] == 1 then
        if unit.move_order ~= nil then
            error(prefix .. " move order")
        end
    else
        expect(unit.move_order, _G[prefix .. "_move"], prefix .. " move order")
    end
    if _G[prefix .. "_fire_nil"] == 1 then
        if unit.fire_order ~= nil then
            error(prefix .. " fire order")
        end
    else
        expect(unit.fire_order, _G[prefix .. "_fire"], prefix .. " fire order")
    end
    expect(unit.kills, _G[prefix .. "_kills"], prefix .. " kills")
end

local function order_row(row, prefix)
    expect(row.kind, _G[prefix .. "_kind"], prefix .. " kind")
    if _G[prefix .. "_target_nil"] == 1 then
        if row.target ~= nil then
            error(prefix .. " target")
        end
    else
        expect(row.target.slot, _G[prefix .. "_target"], prefix .. " target")
    end
    expect(raw_of(row.x, prefix .. " x"), _G[prefix .. "_x"], prefix .. " x")
    expect(raw_of(row.y, prefix .. " y"), _G[prefix .. "_y"], prefix .. " y")
    expect(raw_of(row.z, prefix .. " z"), _G[prefix .. "_z"], prefix .. " z")
    expect(row.parameter_1, _G[prefix .. "_p1"], prefix .. " p1")
    expect(row.parameter_2, _G[prefix .. "_p2"], prefix .. " p2")
end
)lua";

const char* read_fields = R"lua(
local mover = by_slot(mover_slot)
local silo = by_slot(silo_slot)
same_unit(mover, "mover")
same_unit(silo, "silo")

local kind, busy, idle = mover:head_order()
expect(kind, move_kind, "head kind")
expect(flag(busy, "busy"), move_busy, "head busy")
expect(flag(idle, "idle"), move_idle, "head idle")
local primary = mover:orders()
expect(#primary, move_n, "move count")
order_row(primary[1], "move")
local primary_again = mover:orders(nil)
expect(#primary_again, move_n, "orders nil")
expect(primary_again[1].kind, move_kind, "orders nil kind")
expect(#mover:orders(true), move_secondary_n, "mover secondary")
expect(flag(mover:has_background_order(), "mover background"), 0, "mover background")
must_fail(function() mover:orders("no") end, "orders expects true or false")

if silo:head_order() ~= nil then
    error("silo head")
end
expect(#silo:orders(), silo_primary_n, "silo primary")
expect(flag(silo:has_background_order(), "silo background"), 1, "silo background")
local stock = silo:orders(true)
expect(#stock, stock_n, "stock count")
order_row(stock[1], "stock")

local near = {x = fixed.raw(near_x), y = fixed.raw(near_y), z = fixed.raw(near_z)}
local far = {x = fixed.raw(far_x), y = fixed.raw(far_y), z = fixed.raw(far_z)}
expect(flag(silo:reaches(near), "near"), near_hits, "near")
expect(flag(silo:reaches(near, 1), "weapon"), near_hits, "weapon")
expect(flag(silo:reaches(far), "far"), far_hits, "far")
expect(flag(mover:reaches({x = 0, z = 0}), "mover reach"), mover_hits, "mover reach")
must_fail(function() silo:reaches() end, "reaches expects a position: a table with x and z")
must_fail(function() silo:reaches(near, 4) end, "reaches expects a weapon from 1 to 3")

expect(brain.distance(mover, silo):raw(), pair_distance, "unit distance")
expect(brain.direction(mover, silo), pair_direction, "unit direction")
local from = {x = fixed.raw(mover_x), y = fixed.raw(mover_y), z = fixed.raw(mover_z)}
local to = {x = fixed.raw(silo_x), y = fixed.raw(silo_y), z = fixed.raw(silo_z)}
expect(brain.distance(from, to):raw(), pair_distance, "table distance")
expect(brain.direction(from, to), pair_direction, "table direction")
local wide = {x = fixed.raw(wide_x), z = fixed.raw(wide_z)}
local narrow = {x = fixed.raw(narrow_x), z = fixed.raw(narrow_z)}
expect(brain.distance(wide, narrow):raw(), wrap_distance, "wrap distance")
expect(brain.direction(wide, narrow), wrap_direction, "wrap direction")
expect(brain.distance({x = 0, z = 0}, {x = 3, z = 0}):raw(), whole_distance, "whole distance")
expect(brain.direction({x = 0, z = 0}, {x = 3, z = 0}), whole_direction, "whole direction")
must_fail(
    function() brain.distance({x = 40000, z = 0}, {x = 0, z = 0}) end,
    "brain.distance: x must be a fixed value or a whole number")
must_fail(
    function() brain.distance(1, {x = 0, z = 0}) end,
    "brain.distance expects a position: a table with x and z")
must_fail(
    function() brain.distance({z = 0}, {x = 0, z = 0}) end,
    "brain.distance: x must be a fixed value or a whole number")
-- This enemy stands inside the seat's sight, so its seen fields and the
-- geometry read. Own fields stay nil.
seen_unit(enemy, "enemy")
expect(brain.distance(enemy, {x = 0, z = 0}):raw(), enemy_distance, "enemy distance")
expect(brain.direction({x = 0, z = 0}, enemy), enemy_direction, "enemy direction")
nil_private(enemy)

expect(me.player, seat_player, "player")
expect(me.level, level_text, "level")
expect(me.difficulty, difficulty_text, "difficulty")
if type(me.options) ~= "table" then
    error("options")
end
expect(me.name, seat_name, "name")
expect(me.side, seat_side, "side")
expect(me.side_name, seat_side_name, "side name")
expect(me.color, seat_color, "color")
if me.team ~= nil then
    error("team")
end
expect(flag(me.handed_over, "handed"), 0, "handed")
expect(me.refused_assignments, 0, "refused")
if me.knowledge ~= nil then
    error("knowledge")
end
expect(me.tick, match_tick, "tick")
expect(me:unit_count(), listed_n, "unit count")

expect(#me:players(), row_n, "players")
for row = 1, row_n do
    local record = me:players()[row]
    local key = row - 1
    expect(record.player, _G["row_player_" .. key], "row player")
    expect(record.name, _G["row_name_" .. key], "row name")
    expect(record.side, _G["row_side_" .. key], "row side")
    if _G["row_team_nil_" .. key] == 1 then
        if record.team ~= nil then
            error("row team")
        end
    else
        expect(record.team, _G["row_team_" .. key], "row team")
    end
    expect(flag(record.allied, "allied"), _G["row_allied_" .. key], "row allied")
    expect(flag(record.computer, "computer"), _G["row_computer_" .. key], "row computer")
    expect(flag(record.alive, "alive"), _G["row_alive_" .. key], "row alive")
end
for other = 0, 9 do
    expect(flag(me:allied(other), "allied"), _G["allied_" .. other], "allied " .. other)
end
expect(#me:allies(), ally_n, "allies")
for row = 1, ally_n do
    expect(me:allies()[row], _G["ally_" .. (row - 1)], "ally")
end

local walked = {}
me:each_unit(function(unit)
    walked[#walked + 1] = unit.slot
end)
local listed = me:units()
local again = me:units()
expect(#walked, listed_n, "each_unit length")
expect(#listed, listed_n, "units length")
for row = 1, listed_n do
    expect(walked[row], _G["listed_" .. (row - 1)], "listed")
    expect(listed[row].slot, walked[row], "units order")
    if not rawequal(listed[row], again[row]) then
        error("units identity")
    end
    if listed[row] ~= again[row] then
        error("units equal")
    end
end
if getmetatable(listed[1]) ~= false then
    error("metatable")
end
local text = tostring(listed[1])
if string.sub(text, 1, 5) ~= "unit " then
    error("tostring " .. text)
end
must_fail(function() listed[1].health = 1 end, "a unit handle cannot be assigned")
must_fail(function() me.player = 3 end, "me is read-only")
expect(me.player, seat_player, "player unchanged")
must_fail(function() me:squad(10) end, "squad expects a squad from 0 to 9")
must_fail(function() me:squad_size(10) end, "squad_size expects a squad from 0 to 9")
must_fail(function() me:squad_member(10, 1) end, "squad_member expects a squad from 0 to 9")
must_fail(function() me:allied(10) end, "allied expects a player from 0 to 9")
must_fail(function() me:allied("no") end, "allied expects a player from 0 to 9")
must_fail(function() me:each_unit(1) end, "each_unit expects a function")
if me:squad_member(0, "no") ~= nil then
    error("member text")
end
if me:squad_member(0, 1000) ~= nil then
    error("member past end")
end

held = by_slot(spare_slot)
expect(flag(held.alive, "spare"), 1, "spare alive")
expect(held.id, spare_id, "spare id")
expect(held.slot, spare_slot, "spare slot")
)lua";

const char* read_economy = R"lua(
if type(me.energy) ~= "userdata" then
    error("energy type " .. type(me.energy))
end
if type(me.energy_surplus) ~= "number" then
    error("surplus type")
end
expect(me.energy:bits(), res_energy, "energy")
expect(me.metal:bits(), res_metal, "metal")
expect(me.energy_storage:bits(), res_energy_storage, "energy storage")
expect(me.metal_storage:bits(), res_metal_storage, "metal storage")
expect(me.energy_produced:bits(), res_energy_produced, "energy produced")
expect(me.metal_produced:bits(), res_metal_produced, "metal produced")
expect(me.energy_requested:bits(), res_energy_requested, "energy requested")
expect(me.metal_requested:bits(), res_metal_requested, "metal requested")
if me.energy_surplus ~= surplus_energy then
    error("energy surplus")
end
if me.metal_surplus ~= surplus_metal then
    error("metal surplus")
end
expect(me.tick, match_tick, "tick")
local knowledge = me.knowledge
if knowledge == nil then
    error("knowledge")
end
expect(knowledge.builders, know_builders, "builders")
expect(knowledge.tick, know_tick, "know tick")
expect(raw_of(knowledge.base.x, "base x"), base_x, "base x")
expect(raw_of(knowledge.base.y, "base y"), base_y, "base y")
expect(raw_of(knowledge.base.z, "base z"), base_z, "base z")
for type_index = 0, type_count - 1 do
    expect(knowledge:owned(type_index), _G["owned_" .. type_index], "owned")
end
expect(knowledge:owned(type_count), 0, "owned past")
expect(knowledge:owned(-1), 0, "owned negative")
expect(knowledge:owned("no"), 0, "owned text")
)lua";

const char* read_assign = R"lua(
local mover = by_slot(mover_slot)
local collector = by_slot(collector_slot)
local second = by_slot(second_slot)
expect(me:squad_size(1), 0, "squad 1 empty")
if me:assign(mover, 1) ~= true then
    error("assign mover")
end
expect(me:squad_size(1), 1, "squad 1 one")
if me:assign(collector, 1) ~= true then
    error("assign collector")
end
if me:assign(second, 1) ~= true then
    error("assign second")
end
expect(me:squad_size(1), 3, "squad 1 three")
if me:assign(mover, 0) ~= true then
    error("restore mover")
end
expect(me:squad_size(1), 2, "squad 1 two")
)lua";

const char* read_squads = R"lua(
local squad0 = me:squad(0)
expect(#squad0, kept_n, "filtered squad")
for row = 1, kept_n do
    expect(squad0[row].slot, _G["kept_" .. (row - 1)], "kept")
end
expect(me:squad_size(0), raw_n, "raw size")
for row = 1, raw_n do
    local member = me:squad_member(0, row)
    if member == nil then
        error("raw member")
    end
    expect(member.slot, _G["raw_" .. (row - 1)], "raw")
end
if me:squad_member(0, raw_n + 1) ~= nil then
    error("past raw")
end
local squad1 = me:squad(1)
expect(#squad1, squad1_n, "squad 1")
expect(me:squad_size(1), squad1_n, "squad 1 size")
for row = 1, squad1_n do
    expect(squad1[row].slot, _G["squad1_" .. (row - 1)], "squad 1 member")
    local member = me:squad_member(1, row)
    if member == nil then
        error("squad 1 member missing")
    end
    expect(member.slot, squad1[row].slot, "squad 1 raw")
end
)lua";

const char* read_dead = R"lua(
if held.alive ~= false then
    error("dead alive")
end
expect(held.slot, spare_slot, "dead slot")
expect(held.id, spare_id, "dead id")
nil_own(held)
if me:assign(enemy, 0) ~= false then
    error("enemy assign")
end
if me:assign(held, 0) ~= false then
    error("dead assign")
end
must_fail(function() me:assign(held, 10) end, "assign expects a squad from 0 to 9")
must_fail(function() me:assign(1, 0) end, "assign expects a unit")
expect(me.refused_assignments, 2, "refused")
)lua";

const char* read_reuse = R"lua(
local fresh = by_slot(spare_slot)
if rawequal(held, fresh) then
    error("reused same object")
end
if held == fresh then
    error("reused equal")
end
if held.id == fresh.id then
    error("reused id")
end
expect(fresh.id, fresh_id, "fresh id")
expect(fresh.slot, spare_slot, "fresh slot")
expect(flag(fresh.alive, "fresh"), 1, "fresh alive")
if held.alive ~= false then
    error("old alive")
end
nil_own(held)
local again = me:units()
local found = nil
for _, unit in ipairs(again) do
    if unit.slot == spare_slot then
        found = unit
    end
end
if found == nil or not rawequal(found, fresh) then
    error("units cache")
end
)lua";

const char* read_outside = R"lua(
expect(me.player, seat_player, "player")
expect(me.level, level_text, "level")
expect(me.difficulty, difficulty_text, "difficulty")
if type(me.options) ~= "table" then
    error("options")
end
expect(flag(me.handed_over, "handed"), 0, "handed")
if me.name ~= nil then
    error("name")
end
if me.side ~= nil then
    error("side")
end
if me.side_name ~= nil then
    error("side name")
end
if me.team ~= nil then
    error("team")
end
if me.color ~= nil then
    error("color")
end
if me.energy ~= nil then
    error("energy")
end
if me.metal_surplus ~= nil then
    error("surplus")
end
if me.tick ~= nil then
    error("tick")
end
if me.knowledge ~= nil then
    error("knowledge")
end
if me.refused_assignments ~= nil then
    error("refused")
end
expect(#me:units(), 0, "units")
expect(me:unit_count(), 0, "unit count")
expect(#me:squad(0), 0, "squad")
expect(me:squad_size(0), 0, "squad size")
expect(#me:players(), 0, "players")
expect(#me:allies(), 0, "allies")
if me:allied(0) ~= nil then
    error("allied")
end
expect(brain.distance({x = 0, z = 0}, {x = 3, z = 0}):raw(), whole_distance, "whole distance")
expect(brain.direction({x = 0, z = 0}, {x = 3, z = 0}), whole_direction, "whole direction")
local wide = {x = fixed.raw(wide_x), z = fixed.raw(wide_z)}
local narrow = {x = fixed.raw(narrow_x), z = fixed.raw(narrow_z)}
expect(brain.distance(wide, narrow):raw(), wrap_distance, "wrap distance")
expect(brain.direction(wide, narrow), wrap_direction, "wrap direction")
must_fail(
    function() brain.distance({x = 40000, z = 0}, {x = 0, z = 0}) end,
    "brain.distance: x must be a fixed value or a whole number")
must_fail(
    function() brain.distance(1, {x = 0, z = 0}) end,
    "brain.distance expects a position: a table with x and z")
must_fail(function() me:squad(10) end, "squad expects a squad from 0 to 9")
must_fail(function() me:allied(10) end, "allied expects a player from 0 to 9")
expect(ghost.id, ghost_id, "ghost id")
expect(ghost.slot, ghost_slot, "ghost slot")
if ghost.alive ~= nil then
    error("ghost alive")
end
nil_own(ghost)
must_fail(function() me:assign(ghost, 10) end, "assign expects a squad from 0 to 9")
if me:assign(ghost, 0) ~= false then
    error("ghost assign")
end
if me.refused_assignments ~= nil then
    error("refused after assign")
end
)lua";

const char* read_handed = R"lua(
if me.handed_over ~= true then
    error("handed over")
end
expect(me.player, 0, "player")
expect(me.tick, 60, "tick")
)lua";

void run_piece(host::Sandbox& sandbox, const char* body) {
    run_chunk(sandbox, std::string(preamble) + body);
}

void publish_geometry(Published& published) {
    const int32_t wrap_dx = wrapped_difference(narrow_component, wide_component);
    add_number(published, "wide_x", wide_component);
    add_number(published, "wide_z", wide_component);
    add_number(published, "narrow_x", narrow_component);
    add_number(published, "narrow_z", narrow_component);
    add_number(published, "wrap_distance", oa::base::game_math::distance(wrap_dx, wrap_dx));
    add_number(published, "wrap_direction", oa::base::game_math::direction(wrap_dx, wrap_dx));
    const int32_t whole_dx = 3 * 65536;
    add_number(published, "whole_distance", oa::base::game_math::distance(whole_dx, 0));
    add_number(published, "whole_direction", oa::base::game_math::direction(whole_dx, 0));
}

void arm_silo(Fixture& fixture, uint16_t which) {
    oa::Unit* unit = oa::world_unit_at(&fixture.match->state(), which);
    CHECK(unit != nullptr);
    oa::UnitDef* def = oa::world_unit_def_of(&fixture.match->state(), unit);
    CHECK(def != nullptr);
    def->model_height = static_cast<oa_fixed>(256) << 16;
    oa::WeaponDef* weapon = oa::world_weapon_def(&fixture.match->state(), unit->weapons[0].def);
    CHECK(weapon != nullptr);
    weapon->range = 80;
}

void read_the_side(const std::string& script) {
    Fixture fixture;
    const Placed placed = place_units(fixture);
    oa::World& world = fixture.match->state();
    copy_text(
        world.game.players[computer].name, sizeof world.game.players[computer].name, "Side Seat"
    );
    copy_text(world.game.players[0].name, sizeof world.game.players[0].name, "Local Seat");
    world.game.players[computer].team = OA_PLAYER_NO_TEAM;
    world.game.players[0].team = 1;
    oa::PlayerSetupInfo* info = oa::world_player_info(&world, &world.game.players[computer]);
    oa::PlayerSetupInfo* local = oa::world_player_info(&world, &world.game.players[0]);
    CHECK(info != nullptr && local != nullptr);
    info->side = 1;
    info->color = 4;
    local->side = 0;
    local->color = 2;
    copy_text(world.game.sides[1].name, sizeof world.game.sides[1].name, "Ridge");
    copy_text(world.game.sides[0].name, sizeof world.game.sides[0].name, "Arm");
    std::array<uint8_t, 10> allies{};
    allies[computer] = 1;
    allies[0] = 1;
    fixture.match->configure_player_alliances(computer, allies);

    arm_silo(fixture, placed.silo);
    oa::Unit* mover = oa::world_unit_at(&world, placed.mover);
    oa::Unit* silo_unit = oa::world_unit_at(&world, placed.silo);
    CHECK(mover != nullptr && silo_unit != nullptr);
    CHECK((silo_unit->flags & OA_UNIT_FLAG_BUILDING) != 0);
    CHECK((silo_unit->flags & OA_UNIT_FLAG_HAS_WEAPONS) != 0);
    CHECK(fixture.match->takes_move_order(placed.mover));
    const sim::ground_orders::Point destination{
        mover->position.x + (32 << 16), mover->position.y, mover->position.z
    };
    const auto& move = fixture.match->issue_ground_move(placed.mover, destination, false);
    CHECK(!fixture.match->order_refused(move));
    const auto& stocked = fixture.match->issue_build_weapon(placed.silo, 0, 2);
    CHECK(!fixture.match->order_refused(stocked));

    CHECK(sim::ai::configure_match_computer_players(*fixture.match, profile, build_lists, false));
    seat_reader(fixture, script, computer);
    brains::Seat* seat = seat_for(fixture, computer);
    CHECK(!seat->handed_over);
    const auto& computer_slot = fixture.match->computer_player_state();
    CHECK(static_cast<bool>(computer_slot));
    auto* players = static_cast<sim::ai::ComputerPlayers*>(computer_slot.get());
    CHECK(sim::ai::computer_player_knowledge(players, computer) == nullptr);

    const brains::UnitRef mover_ref = brains::unit_ref(*fixture.match, placed.mover);
    CHECK(mover_ref.generation == fixture.match->runtime_state(placed.mover).instance_generation);
    CHECK(brains::unit_current(*fixture.match, mover_ref));
    const uint32_t pool = world.unit_slot_count;
    if (pool > 0 && pool <= 65535u)
        CHECK(brains::unit_generation(*fixture.match, static_cast<uint16_t>(pool)) == 0);
    const brains::UnitRef spare_ref = brains::unit_ref(*fixture.match, placed.spare);
    CHECK(spare_ref.generation == fixture.match->runtime_state(placed.spare).instance_generation);
    const uint32_t old_generation = spare_ref.generation;

    sim::match_runtime::Match::OrderRecordView move_rows[8]{};
    sim::match_runtime::Match::OrderRecordView stock_rows[8]{};
    const std::size_t move_n = fixture.match->queue_records(placed.mover, false, move_rows, 8);
    const std::size_t move_secondary_n =
        fixture.match->queue_records(placed.mover, true, move_rows + 1, 7);
    const std::size_t silo_primary_n =
        fixture.match->queue_records(placed.silo, false, stock_rows, 8);
    const std::size_t stock_n = fixture.match->queue_records(placed.silo, true, stock_rows, 8);
    CHECK(move_n >= 1);
    CHECK(move_secondary_n == 0);
    CHECK(silo_primary_n == 0);
    CHECK(stock_n >= 1);
    CHECK(stock_rows[0].parameter_1 == 0);
    CHECK(stock_rows[0].parameter_2 == 2);
    CHECK(fixture.match->orders(placed.mover).primary != nullptr);
    CHECK(fixture.match->orders(placed.mover).secondary == nullptr);
    CHECK(fixture.match->orders(placed.silo).secondary != nullptr);
    const bool busy = (move_rows[0].preserve_flags & order_preserve_busy) != 0;
    const uint8_t command = fixture.match->primary_order_command_flags(placed.mover);
    const bool idle = (command & order_queue_idle) != 0;
    CHECK(idle == ((move_rows[0].command_flags & order_queue_idle) != 0));

    oa::FixedVec3 near{};
    near.x = static_cast<int32_t>(static_cast<uint32_t>(silo_unit->position.x) + (8u << 16));
    near.y = silo_unit->position.y;
    near.z = silo_unit->position.z;
    oa::FixedVec3 far{};
    far.x = static_cast<int32_t>(static_cast<uint32_t>(silo_unit->position.x) + (400u << 16));
    far.y = silo_unit->position.y;
    far.z = silo_unit->position.z;
    const auto rules = fixture.match->rules_view();
    const bool near_hits =
        sim::weapon_execution::slot_reaches_point(world, *silo_unit, near, 0, rules);
    const bool far_hits =
        sim::weapon_execution::slot_reaches_point(world, *silo_unit, far, 0, rules);
    CHECK(near_hits);
    CHECK(!far_hits);
    oa::FixedVec3 origin{};
    const bool mover_hits =
        sim::weapon_execution::slot_reaches_point(world, *mover, origin, 0, rules);

    const int32_t pair_dx = wrapped_difference(silo_unit->position.x, mover->position.x);
    const int32_t pair_dz = wrapped_difference(silo_unit->position.z, mover->position.z);

    Published published;
    add_number(published, "mover_slot", placed.mover);
    add_number(published, "silo_slot", placed.silo);
    add_number(published, "collector_slot", placed.collector);
    add_number(published, "second_slot", placed.second);
    add_number(published, "spare_slot", placed.spare);
    add_number(published, "mover_id", handle_id(mover_ref));
    add_number(published, "spare_id", handle_id(spare_ref));
    add_number(published, "mover_alive", 1);
    add_number(published, "silo_alive", 1);
    const brains::UnitRef silo_ref = brains::unit_ref(*fixture.match, placed.silo);
    add_number(published, "silo_id", handle_id(silo_ref));
    publish_unit(published, "mover", world, *mover);
    publish_unit(published, "silo", world, *silo_unit);
    publish_order(published, "move", move_rows[0]);
    publish_order(published, "stock", stock_rows[0]);
    add_number(published, "move_n", static_cast<lua_Integer>(move_n));
    add_number(published, "move_secondary_n", static_cast<lua_Integer>(move_secondary_n));
    add_number(published, "silo_primary_n", static_cast<lua_Integer>(silo_primary_n));
    add_number(published, "stock_n", static_cast<lua_Integer>(stock_n));
    add_number(published, "move_busy", busy ? 1 : 0);
    add_number(published, "move_idle", idle ? 1 : 0);
    add_number(published, "near_x", near.x);
    add_number(published, "near_y", near.y);
    add_number(published, "near_z", near.z);
    add_number(published, "far_x", far.x);
    add_number(published, "far_y", far.y);
    add_number(published, "far_z", far.z);
    add_number(published, "near_hits", near_hits ? 1 : 0);
    add_number(published, "far_hits", far_hits ? 1 : 0);
    add_number(published, "mover_hits", mover_hits ? 1 : 0);
    add_number(published, "pair_distance", oa::base::game_math::distance(pair_dx, pair_dz));
    add_number(published, "pair_direction", oa::base::game_math::direction(pair_dx, pair_dz));
    publish_geometry(published);

    const brains::UnitRef enemy_ref = brains::unit_ref(*fixture.match, placed.enemy);
    oa::Unit* enemy_unit = oa::world_unit_at(&world, placed.enemy);
    CHECK(enemy_unit != nullptr);
    // The seat's relay at cell 16,16 has sight 200, which covers this unit at
    // cell 8,8, so seen fields and brain.distance read it.
    CHECK(fixture.match->unit_visible(computer, placed.enemy));
    publish_unit(published, "enemy", world, *enemy_unit);
    add_number(published, "enemy_id", handle_id(enemy_ref));
    add_number(published, "enemy_slot", placed.enemy);
    // Distance is the enemy toward the origin. Direction is the origin toward
    // the enemy, so the two headings are opposite.
    const int32_t toward_origin_dx = wrapped_difference(0, enemy_unit->position.x);
    const int32_t toward_origin_dz = wrapped_difference(0, enemy_unit->position.z);
    const int32_t toward_enemy_dx = wrapped_difference(enemy_unit->position.x, 0);
    const int32_t toward_enemy_dz = wrapped_difference(enemy_unit->position.z, 0);
    add_number(
        published,
        "enemy_distance",
        oa::base::game_math::distance(toward_origin_dx, toward_origin_dz)
    );
    add_number(
        published,
        "enemy_direction",
        oa::base::game_math::direction(toward_enemy_dx, toward_enemy_dz)
    );
    add_number(published, "seat_player", computer);
    add_view(published, "level_text", "medium");
    add_view(published, "difficulty_text", "medium");
    add_view(published, "seat_name", "Side Seat");
    add_view(published, "seat_side_name", "Ridge");
    add_number(published, "seat_side", info->side);
    add_number(published, "seat_color", info->color);
    add_number(published, "match_tick", world.game.tick);

    int listed_n = 0;
    uint32_t span = 0;
    const oa::Unit* first = oa::world_player_units(&world, &world.game.players[computer], &span);
    CHECK(first != nullptr);
    for (uint32_t step = 0; step < span; ++step) {
        if (first[step].type_index == 0)
            continue;
        char label[name_room];
        std::snprintf(label, sizeof label, "listed_%d", listed_n);
        add_number(published, label, first[step].id);
        ++listed_n;
    }
    add_number(published, "listed_n", listed_n);
    CHECK(listed_n == 6);

    int row_n = 0;
    bool saw_local = false;
    bool saw_computer = false;
    for (uint8_t which = 0; which < OA_PLAYER_COUNT; ++which) {
        const oa::Player& record = world.game.players[which];
        if (record.in_use == 0)
            continue;
        char label[name_room];
        std::snprintf(label, sizeof label, "row_player_%d", row_n);
        add_number(published, label, which);
        std::snprintf(label, sizeof label, "row_name_%d", row_n);
        add_view(published, label, std::string_view(record.name, sizeof record.name));
        const oa::PlayerSetupInfo* setup = oa::world_player_info(&world, &record);
        CHECK(setup != nullptr);
        std::snprintf(label, sizeof label, "row_side_%d", row_n);
        add_number(published, label, setup->side);
        std::snprintf(label, sizeof label, "row_team_nil_%d", row_n);
        add_number(published, label, record.team == OA_PLAYER_NO_TEAM ? 1 : 0);
        std::snprintf(label, sizeof label, "row_team_%d", row_n);
        add_number(published, label, record.team);
        std::snprintf(label, sizeof label, "row_allied_%d", row_n);
        add_number(published, label, fixture.match->allied(computer, which) ? 1 : 0);
        std::snprintf(label, sizeof label, "row_computer_%d", row_n);
        const bool machine = record.status == OA_PLAYER_STATUS_COMPUTER;
        add_number(published, label, machine ? 1 : 0);
        std::snprintf(label, sizeof label, "row_alive_%d", row_n);
        add_number(published, label, record.unit_count > 0 ? 1 : 0);
        if (which == 0) {
            saw_local = true;
            CHECK(!machine);
        }
        if (which == computer) {
            saw_computer = true;
            CHECK(machine);
            CHECK(record.unit_count > 0);
        }
        ++row_n;
    }
    add_number(published, "row_n", row_n);
    CHECK(row_n == 2 && saw_local && saw_computer);

    int ally_n = 0;
    for (uint8_t which = 0; which < OA_PLAYER_COUNT; ++which) {
        char label[name_room];
        std::snprintf(label, sizeof label, "allied_%u", static_cast<unsigned>(which));
        const bool allied = fixture.match->allied(computer, which);
        add_number(published, label, allied ? 1 : 0);
        if (which == computer || !allied)
            continue;
        std::snprintf(label, sizeof label, "ally_%d", ally_n);
        add_number(published, label, which);
        ++ally_n;
    }
    add_number(published, "ally_n", ally_n);
    CHECK(fixture.match->allied(computer, computer));
    CHECK(fixture.match->allied(computer, 0));
    CHECK(ally_n == 1);

    host::Sandbox* sandbox = brains::seat_sandbox(*fixture.match, computer);
    CHECK(sandbox != nullptr);
    publish(*sandbox, published);
    push_global(*sandbox, *seat, enemy_ref, "enemy");
    run_piece(*sandbox, read_fields);

    for (uint32_t step = 0; step < 300; ++step) {
        ++world.game.tick;
        fixture.match->tick();
    }
    const oa::Player& economy = world.game.players[computer];
    const double energy_surplus = oa::player_energy_surplus(&economy);
    const double metal_surplus = oa::player_metal_surplus(&economy);
    CHECK(
        energy_surplus ==
        static_cast<double>(economy.energy_produced) - static_cast<double>(economy.energy_requested)
    );
    CHECK(
        metal_surplus ==
        static_cast<double>(economy.metal_produced) - static_cast<double>(economy.metal_requested)
    );
    clear_published(published);
    add_number(published, "res_energy", bits_of(economy.energy));
    add_number(published, "res_metal", bits_of(economy.metal));
    add_number(published, "res_energy_storage", bits_of(economy.energy_storage));
    add_number(published, "res_metal_storage", bits_of(economy.metal_storage));
    add_number(published, "res_energy_produced", bits_of(economy.energy_produced));
    add_number(published, "res_metal_produced", bits_of(economy.metal_produced));
    add_number(published, "res_energy_requested", bits_of(economy.energy_requested));
    add_number(published, "res_metal_requested", bits_of(economy.metal_requested));
    add_double(published, "surplus_energy", energy_surplus);
    add_double(published, "surplus_metal", metal_surplus);
    add_number(published, "match_tick", world.game.tick);
    CHECK(static_cast<bool>(computer_slot));
    players = static_cast<sim::ai::ComputerPlayers*>(computer_slot.get());
    sim::ai::ComputerKnowledge* knowledge = sim::ai::computer_player_knowledge(players, computer);
    CHECK(knowledge != nullptr);
    CHECK(knowledge->builder_count >= 1);
    const uint32_t refreshed = fixture.match->sightings(computer).refreshed_tick;
    CHECK(refreshed != 0);
    add_number(published, "know_builders", knowledge->builder_count);
    add_number(published, "know_tick", refreshed);
    add_number(published, "base_x", knowledge->base_position.x);
    add_number(published, "base_y", knowledge->base_position.y);
    add_number(published, "base_z", knowledge->base_position.z);
    const uint32_t type_count = players->type_count;
    CHECK(type_count >= 2 && type_count <= 32);
    add_number(published, "type_count", type_count);
    lua_Integer owned_sum = 0;
    for (uint32_t type_index = 0; type_index < type_count; ++type_index) {
        char label[name_room];
        std::snprintf(label, sizeof label, "owned_%u", type_index);
        const lua_Integer owned =
            knowledge->owned_counts == nullptr ? 0 : knowledge->owned_counts[type_index];
        add_number(published, label, owned);
        owned_sum += owned;
    }
    CHECK(owned_sum > 0);
    publish(*sandbox, published);
    run_piece(*sandbox, read_economy);

    run_piece(*sandbox, read_assign);
    oa::Unit* quirk = oa::world_unit_at(&world, placed.quirk);
    CHECK(quirk != nullptr);
    // The restore path writes Unit.squad before set_unit_squad, so the unit stays
    // in squad 0's list. Plant that and do not call set_unit_squad.
    quirk->squad = 1;

    uint16_t raw_members[16]{};
    std::size_t raw_n = 0;
    {
        const auto members = fixture.match->squad_members(computer, 0);
        CHECK(members.size() <= 16);
        for (const uint16_t member : members) {
            raw_members[raw_n] = member;
            ++raw_n;
        }
    }
    uint16_t kept_members[16]{};
    std::size_t kept_n = 0;
    bool quirk_raw = false;
    bool quirk_kept = false;
    for (std::size_t row = 0; row < raw_n; ++row) {
        if (raw_members[row] == placed.quirk)
            quirk_raw = true;
        const oa::Unit* unit = oa::world_unit_at(&world, raw_members[row]);
        if (unit == nullptr || unit->type_index == 0 || !oa::unit_is_live_target(unit->flags) ||
            unit->squad != 0)
            continue;
        if (raw_members[row] == placed.quirk)
            quirk_kept = true;
        kept_members[kept_n] = raw_members[row];
        ++kept_n;
    }
    CHECK(quirk_raw);
    CHECK(!quirk_kept);
    CHECK(kept_n + 1 == raw_n);
    uint16_t squad1_members[16]{};
    std::size_t squad1_n = 0;
    {
        const auto members = fixture.match->squad_members(computer, 1);
        CHECK(members.size() <= 16);
        for (const uint16_t member : members) {
            squad1_members[squad1_n] = member;
            ++squad1_n;
        }
    }
    CHECK(squad1_n == 2);
    clear_published(published);
    add_number(published, "kept_n", static_cast<lua_Integer>(kept_n));
    add_number(published, "raw_n", static_cast<lua_Integer>(raw_n));
    add_number(published, "squad1_n", static_cast<lua_Integer>(squad1_n));
    for (std::size_t row = 0; row < kept_n; ++row) {
        char label[name_room];
        std::snprintf(label, sizeof label, "kept_%u", static_cast<unsigned>(row));
        add_number(published, label, kept_members[row]);
    }
    for (std::size_t row = 0; row < raw_n; ++row) {
        char label[name_room];
        std::snprintf(label, sizeof label, "raw_%u", static_cast<unsigned>(row));
        add_number(published, label, raw_members[row]);
    }
    for (std::size_t row = 0; row < squad1_n; ++row) {
        char label[name_room];
        std::snprintf(label, sizeof label, "squad1_%u", static_cast<unsigned>(row));
        add_number(published, label, squad1_members[row]);
    }
    publish(*sandbox, published);
    run_piece(*sandbox, read_squads);

    fixture.match->kill_unit(
        placed.spare, static_cast<uint8_t>(sim::match_runtime::DeathKind::dismissed)
    );
    CHECK(!brains::unit_current(*fixture.match, spare_ref));
    run_piece(*sandbox, read_dead);
    CHECK(seat->refused_assignments == 2);

    auto* created = fixture.match->create(
        {computer, vehicle, {cell_at(28), 0u, cell_at(28)}, true, 1u, placed.spare}, 0
    );
    CHECK(created != nullptr);
    CHECK(created->unit_index == placed.spare);
    const uint32_t fresh_generation = brains::unit_generation(*fixture.match, placed.spare);
    CHECK(fresh_generation != old_generation);
    CHECK(fresh_generation == fixture.match->runtime_state(placed.spare).instance_generation);
    const brains::UnitRef fresh_ref{placed.spare, fresh_generation};
    CHECK(brains::unit_current(*fixture.match, fresh_ref));
    CHECK(!brains::unit_current(*fixture.match, spare_ref));
    clear_published(published);
    add_number(published, "fresh_id", handle_id(fresh_ref));
    add_number(published, "spare_slot", placed.spare);
    publish(*sandbox, published);
    run_piece(*sandbox, read_reuse);
}

void handed_over_after_the_start(const std::string& script) {
    Fixture fixture;
    CHECK(sim::ai::configure_match_computer_players(*fixture.match, profile, build_lists, false));
    for (uint32_t step = 0; step < 60; ++step) {
        ++fixture.match->state().game.tick;
        fixture.match->tick();
    }
    const auto& computer_slot = fixture.match->computer_player_state();
    CHECK(static_cast<bool>(computer_slot));
    const auto* players = static_cast<const sim::ai::ComputerPlayers*>(computer_slot.get());
    CHECK(players->initialized != 0);
    oa::Player& local = fixture.match->state().game.players[0];
    CHECK(local.in_use != 0);
    local.status = OA_PLAYER_STATUS_COMPUTER;
    seat_reader(fixture, script, 0);
    brains::Seat* seat = seat_for(fixture, 0);
    CHECK(seat->handed_over);
    host::Sandbox* sandbox = brains::seat_sandbox(*fixture.match, 0);
    CHECK(sandbox != nullptr);
    run_piece(*sandbox, read_handed);
}

void outside_a_match() {
    brains::Seat seat;
    seat.player = computer;
    copy_text(seat.level, sizeof seat.level, "medium");
    copy_text(seat.difficulty, sizeof seat.difficulty, "medium");
    const host::Bank bank{
        host::default_tick_allowance, host::default_bank_ticks, host::default_call_time_limit
    };
    host::OpenOptions options;
    options.package_id = "side-reader";
    options.profile = brains::brain_profile(seat, bank);
    options.sources = seat.sources.hooks();
    options.random_seed = 0;
    host::ScriptError error;
    auto opened = host::Sandbox::open(options, error);
    if (!opened.has_value())
        throw std::runtime_error(error.message.empty() ? "open failed" : error.message);
    host::Sandbox sandbox(std::move(*opened));

    Published published;
    add_number(published, "seat_player", computer);
    add_view(published, "level_text", "medium");
    add_view(published, "difficulty_text", "medium");
    publish_geometry(published);
    const brains::UnitRef ghost{1, 1};
    add_number(published, "ghost_id", handle_id(ghost));
    add_number(published, "ghost_slot", ghost.slot);
    publish(sandbox, published);
    push_global(sandbox, seat, ghost, "ghost");
    run_piece(sandbox, read_outside);
    CHECK(seat.refused_assignments == 1);
}

void two_seated_runs(const std::string& script) {
    // A seated brain replaces the built-in player's order tick (B01), so a run
    // with no brain does not share that run's random stream. Two seated runs,
    // one of which reads the side after every tick, do.
    Fixture quiet;
    Fixture reading;
    place_units(quiet);
    place_units(reading);
    CHECK(sim::ai::configure_match_computer_players(*quiet.match, profile, build_lists, false));
    CHECK(sim::ai::configure_match_computer_players(*reading.match, profile, build_lists, false));
    seat_reader(quiet, script, computer);
    seat_reader(reading, script, computer);
    CHECK(quiet.match->random_state() == reading.match->random_state());
    host::Sandbox* sandbox = brains::seat_sandbox(*reading.match, computer);
    CHECK(sandbox != nullptr);
    for (uint32_t step = 0; step < run_ticks; ++step) {
        ++quiet.match->state().game.tick;
        quiet.match->tick();
        ++reading.match->state().game.tick;
        reading.match->tick();
        run_chunk(*sandbox, "read_all()\n");
        if (quiet.match->random_state() != reading.match->random_state())
            throw std::runtime_error("random diverged at tick " + std::to_string(step + 1));
    }
    const oa::base::game_loop::Timing timing{};
    const uint64_t quiet_hash = sim::trace::match_state_hash(*quiet.match, timing, 0, 0, nullptr);
    const uint64_t reading_hash =
        sim::trace::match_state_hash(*reading.match, timing, 0, 0, nullptr);
    CHECK(quiet_hash == reading_hash);
}

} // namespace

int main() {
    try {
        const std::string script = load_script();
        read_the_side(script);
        handed_over_after_the_start(script);
        outside_a_match();
        two_seated_runs(script);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "sim-brains-me: %s\n", ex.what());
        return 1;
    }
    return 0;
}
