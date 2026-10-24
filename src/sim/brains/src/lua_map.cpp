// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// game.map: the match map a brain can read, and whether two points lie in
// one movement region. A question does not search for a path and does not
// change the movement map.
#include "internal.hpp"
#include "reach.hpp"

#include "handles_internal.hpp"
#include "lua_game.hpp"

#include "oa/core/map_plot.h"
#include "oa/core/unit.h"
#include "oa/core/unit_def.h"
#include "oa/core/world.h"
#include "oa/data/script_host/f32.hpp"
#include "oa/data/script_host/fixed.hpp"
#include "oa/data/script_host/script_host.hpp"
#include "oa/sim/brains/handles.hpp"
#include "oa/sim/match_runtime.hpp"
#include "oa/sim/spatial_state/spatial.hpp"

#include "lua.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace oa::sim::brains {
namespace {

constexpr const char* map_meta = "oa.brain.map";
constexpr const char* unit_record_meta = "oa.brain.unit_def";
constexpr const char* unit_names = "oa.brain.unit_names";
constexpr std::size_t name_room = 128;
constexpr std::size_t most_classes = 32;
constexpr uint64_t reach_charge = 16;
constexpr int64_t component_min = int64_t{-2147483647} - 1;
constexpr int64_t component_max = 2147483647;
constexpr lua_Integer whole_min = -32768;
constexpr lua_Integer whole_max = 32767;

// Same layout as the unit-record userdata the definition library pushes.
struct UnitRecord {
    uint8_t kind{};
    uint16_t index{};
};

struct KnownClass {
    uint32_t handle{};
    uint16_t type{};
};

enum class ClassKind : uint8_t { ground, aircraft };

struct ClassUse {
    ClassKind kind{ClassKind::ground};
    uint32_t handle{};
    const sim::ground_orders::MovementMap* grid{};
    int16_t footprint_x{};
    int16_t footprint_z{};
};

struct SightView {
    const sim::ground_orders::MovementMap* grid{};
    const uint16_t* bits{};
    std::size_t bit_count{};
    uint32_t world_width{};
    uint32_t world_height{};
    uint32_t player{};
};

struct Endpoint {
    bool hidden{};
    int32_t x{};
    int32_t z{};
};

bool same_text(const char* text, std::size_t length, const char* literal) noexcept {
    if (text == nullptr || literal == nullptr)
        return false;
    const std::size_t literal_length = std::strlen(literal);
    return length == literal_length && std::memcmp(text, literal, length) == 0;
}

bool names_equal(
    const char* left, std::size_t left_length, const char* right, std::size_t right_length
) noexcept {
    if (left == nullptr || right == nullptr || left_length != right_length)
        return false;
    for (std::size_t i = 0; i < left_length; ++i) {
        auto a = static_cast<unsigned char>(left[i]);
        auto b = static_cast<unsigned char>(right[i]);
        if (a >= 'A' && a <= 'Z')
            a = static_cast<unsigned char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z')
            b = static_cast<unsigned char>(b - 'A' + 'a');
        if (a != b)
            return false;
    }
    return true;
}

void copy_text(char* dest, std::size_t room, const char* text, std::size_t length) noexcept {
    if (dest == nullptr || room == 0)
        return;
    std::size_t count = length;
    if (count + 1 > room)
        count = room - 1;
    if (text != nullptr && count > 0)
        std::memcpy(dest, text, count);
    dest[count] = '\0';
}

void copy_decimal(char* dest, std::size_t room, lua_Integer number) noexcept {
    char text[32];
    std::snprintf(text, sizeof text, "%lld", static_cast<long long>(number));
    copy_text(dest, room, text, std::strlen(text));
}

bool map_self(lua_State* lua, const char* method) {
    if (luaL_testudata(lua, 1, map_meta) == nullptr) {
        luaL_error(lua, "map:%s expects the map", method);
        return false;
    }
    return true;
}

Seat* live_seat(lua_State* lua) noexcept {
    Seat* seat = seat_of(lua);
    if (seat == nullptr || seat->match == nullptr)
        return nullptr;
    return seat;
}

bool read_cell(lua_State* lua, int index, const char* method, int32_t* out) {
    if (lua_isinteger(lua, index) == 0) {
        luaL_error(lua, "map:%s: a cell must be an integer", method);
        return false;
    }
    const lua_Integer value = lua_tointeger(lua, index);
    if (value < static_cast<lua_Integer>(INT32_MIN) ||
        value > static_cast<lua_Integer>(INT32_MAX)) {
        *out = -1;
        return true;
    }
    *out = static_cast<int32_t>(value);
    return true;
}

const sim::spatial_state::Plot*
plot_at(const match_runtime::Match& match, int32_t x, int32_t z) noexcept {
    const auto& spatial = match.spatial();
    if (x < 0 || z < 0 || static_cast<uint32_t>(x) >= spatial.terrain_width ||
        static_cast<uint32_t>(z) >= spatial.terrain_height)
        return nullptr;
    const auto index =
        static_cast<std::size_t>(z) * spatial.terrain_width + static_cast<std::size_t>(x);
    if (index >= spatial.plots.size())
        return nullptr;
    return &spatial.plots[index];
}

void remember_type_name(
    const oa::World& world, uint16_t type, char* name, std::size_t room
) noexcept {
    if (world.unit_defs == nullptr || type >= world.unit_def_count) {
        copy_decimal(name, room, static_cast<lua_Integer>(type));
        return;
    }
    const char* text = world.unit_defs[type].unit_name;
    std::size_t length = 0;
    while (length < sizeof world.unit_defs[type].unit_name && text[length] != '\0')
        ++length;
    if (length == 0)
        copy_decimal(name, room, static_cast<lua_Integer>(type));
    else
        copy_text(name, room, text, length);
}

uint32_t
collect_classes(const match_runtime::Match& match, KnownClass* out, uint32_t room) noexcept {
    const oa::World& world = match.state();
    uint32_t count = 0;
    if (world.unit_defs == nullptr || out == nullptr)
        return 0;
    for (uint32_t type = 1; type < world.unit_def_count; ++type) {
        const uint32_t handle = world.unit_defs[type].move_class;
        if (handle == 0)
            continue;
        bool seen = false;
        for (uint32_t held = 0; held < count; ++held) {
            if (out[held].handle == handle)
                seen = true;
        }
        if (seen || count >= room)
            continue;
        out[count].handle = handle;
        out[count].type = static_cast<uint16_t>(type);
        ++count;
    }
    for (uint32_t i = 1; i < count; ++i) {
        const KnownClass item = out[i];
        uint32_t slot = i;
        while (slot > 0 && out[slot - 1].handle > item.handle) {
            out[slot] = out[slot - 1];
            --slot;
        }
        out[slot] = item;
    }
    return count;
}

bool class_named(
    const match_runtime::Match& match,
    const KnownClass* known,
    uint32_t count,
    const char* text,
    std::size_t length,
    KnownClass* found
) noexcept {
    for (uint32_t i = 0; i < count; ++i) {
        const auto* definition = match.unit_definition(known[i].type);
        if (definition == nullptr)
            continue;
        const char* label = definition->movement_class.data();
        const std::size_t label_length = definition->movement_class.size();
        if (!names_equal(label, label_length, text, length))
            continue;
        *found = known[i];
        return true;
    }
    return false;
}

bool fold_name(char* dest, std::size_t room, const char* text, std::size_t length) noexcept {
    if (dest == nullptr || text == nullptr || length + 1 > room)
        return false;
    for (std::size_t i = 0; i < length; ++i) {
        const auto letter = static_cast<unsigned char>(text[i]);
        const auto lower = letter >= 'A' && letter <= 'Z' ? letter - 'A' + 'a' : letter;
        dest[i] = static_cast<char>(lower);
    }
    dest[length] = '\0';
    return true;
}

bool lookup_unit_name(lua_State* lua, const char* text, std::size_t length, uint16_t* index) {
    char folded[name_room];
    if (index == nullptr || !fold_name(folded, sizeof folded, text, length))
        return false;
    lua_getfield(lua, LUA_REGISTRYINDEX, unit_names);
    if (!lua_istable(lua, -1)) {
        lua_pop(lua, 1);
        return false;
    }
    lua_pushlstring(lua, folded, length);
    lua_rawget(lua, -2);
    const bool found = lua_isinteger(lua, -1) != 0;
    if (found)
        *index = static_cast<uint16_t>(lua_tointeger(lua, -1));
    lua_pop(lua, 2);
    return found;
}

bool type_in_table(const oa::World& world, uint16_t type) noexcept {
    return world.unit_defs != nullptr && type >= 1 && type < world.unit_def_count;
}

void use_type(const match_runtime::Match& match, uint16_t type, ClassUse* out) noexcept {
    const oa::World& world = match.state();
    const uint32_t handle = world.unit_defs[type].move_class;
    if (handle == 0) {
        out->kind = ClassKind::aircraft;
        out->handle = 0;
        out->grid = nullptr;
        return;
    }
    out->kind = ClassKind::ground;
    out->handle = handle;
    out->grid = match.movement_map(static_cast<sim::unit_spawn::AssetHandle>(handle));
    if (out->grid != nullptr) {
        out->footprint_x = out->grid->footprint_x();
        out->footprint_z = out->grid->footprint_z();
    }
}

bool refuse_class(lua_State* lua, const char* name) {
    luaL_error(lua, "unknown movement class: %s", name == nullptr ? "" : name);
    return false;
}

bool resolve_class(
    lua_State* lua, Seat& seat, int index, char* name, std::size_t room, ClassUse* out
) {
    if (name != nullptr && room > 0)
        name[0] = '\0';
    const oa::World& world = seat.match->state();
    KnownClass known[most_classes]{};
    const uint32_t count = collect_classes(*seat.match, known, most_classes);
    auto* record = static_cast<UnitRecord*>(luaL_testudata(lua, index, unit_record_meta));
    if (record != nullptr) {
        if (!type_in_table(world, record->index)) {
            copy_decimal(name, room, static_cast<lua_Integer>(record->index));
            return refuse_class(lua, name);
        }
        remember_type_name(world, record->index, name, room);
        use_type(*seat.match, record->index, out);
        return true;
    }
    if (lua_type(lua, index) == LUA_TSTRING) {
        std::size_t length = 0;
        const char* text = lua_tolstring(lua, index, &length);
        copy_text(name, room, text, length);
        KnownClass found{};
        if (class_named(*seat.match, known, count, text, length, &found)) {
            use_type(*seat.match, found.type, out);
            return true;
        }
        uint16_t type = 0;
        if (text != nullptr && lookup_unit_name(lua, text, length, &type) &&
            type_in_table(world, type)) {
            remember_type_name(world, type, name, room);
            use_type(*seat.match, type, out);
            return true;
        }
        return refuse_class(lua, name);
    }
    if (lua_isinteger(lua, index) != 0) {
        const lua_Integer number = lua_tointeger(lua, index);
        copy_decimal(name, room, number);
        if (number > 0 && static_cast<uint64_t>(number) <= UINT32_MAX) {
            const auto handle = static_cast<uint32_t>(number);
            for (uint32_t i = 0; i < count; ++i) {
                if (known[i].handle != handle)
                    continue;
                remember_type_name(world, known[i].type, name, room);
                use_type(*seat.match, known[i].type, out);
                return true;
            }
        }
        if (number >= 1 && number <= 0xffff &&
            type_in_table(world, static_cast<uint16_t>(number))) {
            const auto type = static_cast<uint16_t>(number);
            remember_type_name(world, type, name, room);
            use_type(*seat.match, type, out);
            return true;
        }
        return refuse_class(lua, name);
    }
    UnitRef ignored{};
    if (to_unit(lua, index, &ignored))
        copy_text(name, room, "unit", 4);
    else if (lua_type(lua, index) == LUA_TNIL)
        copy_text(name, room, "nil", 3);
    else
        copy_text(name, room, "value", 5);
    return refuse_class(lua, name);
}

bool read_fixed_arg(lua_State* lua, int index, const char* method, const char* axis, int32_t* out) {
    int64_t count = 0;
    if (script_host::to_fixed(lua, index, &count)) {
        if (count < component_min || count > component_max) {
            luaL_error(lua, "map:%s: %s is outside -32768 to 32767.99998", method, axis);
            return false;
        }
        *out = static_cast<int32_t>(count);
        return true;
    }
    if (lua_isinteger(lua, index) != 0) {
        const lua_Integer whole = lua_tointeger(lua, index);
        if (whole < whole_min || whole > whole_max) {
            luaL_error(lua, "map:%s: %s is outside -32768 to 32767.99998", method, axis);
            return false;
        }
        *out = static_cast<int32_t>(static_cast<int64_t>(whole) * int64_t{65536});
        return true;
    }
    luaL_error(lua, "map:%s: %s must be a fixed value or an integer", method, axis);
    return false;
}

bool metal_spot(const sim::spatial_state::Plot& plot) noexcept {
    return plot.feature_word < OA_PLOT_FEATURE_RESERVED && plot.metal_feature &&
           plot.indestructible_feature;
}

bool geo_spot(const sim::spatial_state::Plot& plot) noexcept {
    return plot.geo_feature && plot.feature_word < sim::spatial_state::first_reserved_feature;
}

using SpotTest = bool (*)(const sim::spatial_state::Plot& plot);

int push_spots(lua_State* lua, const match_runtime::Match& match, SpotTest test) {
    const auto& spatial = match.spatial();
    lua_newtable(lua);
    const int list = lua_gettop(lua);
    lua_Integer row = 1;
    uint64_t scanned = 0;
    if (spatial.terrain_width > 0 && spatial.terrain_height > 0) {
        for (uint32_t z = 0; z < spatial.terrain_height; ++z) {
            for (uint32_t x = 0; x < spatial.terrain_width; ++x) {
                ++scanned;
                const sim::spatial_state::Plot* plot =
                    plot_at(match, static_cast<int32_t>(x), static_cast<int32_t>(z));
                if (plot == nullptr || !test(*plot))
                    continue;
                lua_createtable(lua, 0, 2);
                lua_pushinteger(lua, static_cast<lua_Integer>(x));
                lua_setfield(lua, -2, "cx");
                lua_pushinteger(lua, static_cast<lua_Integer>(z));
                lua_setfield(lua, -2, "cz");
                lua_rawseti(lua, list, row);
                ++row;
            }
        }
    }
    script_host::charge_work(lua, scanned * script_host::element_charge);
    return 1;
}

int32_t surface_metal(const match_runtime::Match& match) noexcept {
    for (const sim::spatial_state::Plot& plot : match.spatial().plots) {
        if (!plot.metal_feature)
            return plot.metal;
    }
    return 0;
}

bool sight_pass(void* context, uint32_t x, uint32_t z) {
    auto* sight = static_cast<SightView*>(context);
    if (sight == nullptr || sight->grid == nullptr)
        return false;
    if (x >= sight->grid->width() || z >= sight->grid->height())
        return false;
    const auto coarse_x =
        (static_cast<int32_t>(sight->grid->footprint_x()) >> 2) + (static_cast<int32_t>(x) >> 1);
    const auto coarse_z =
        (static_cast<int32_t>(sight->grid->footprint_z()) >> 2) + (static_cast<int32_t>(z) >> 1);
    const auto coarse_w = static_cast<int32_t>(sight->world_width) >> 1;
    const auto coarse_h = static_cast<int32_t>(sight->world_height) >> 1;
    if (coarse_x < 0 || coarse_z < 0 || coarse_x >= coarse_w || coarse_z >= coarse_h)
        return false;
    const auto index = static_cast<std::size_t>(coarse_w) * static_cast<std::size_t>(coarse_z) +
                       static_cast<std::size_t>(coarse_x);
    if (index >= sight->bit_count || sight->bits == nullptr)
        return false;
    const auto bits = static_cast<uint32_t>(sight->bits[index]);
    if (((1u << (sight->player & 31u)) & bits) == 0u)
        return true;
    return sight->grid->cell(x, z) >= 1;
}

bool read_endpoint(lua_State* lua, Seat& seat, int index, Endpoint* out) {
    UnitRef ref{};
    if (to_unit(lua, index, &ref)) {
        if (unit_access(seat, ref, Access::seen) != Read::allow) {
            out->hidden = true;
            return true;
        }
        const oa::Unit* unit = oa::world_unit_at(&seat.match->state(), ref.slot);
        if (unit == nullptr) {
            out->hidden = true;
            return true;
        }
        out->x = unit->position.x;
        out->z = unit->position.z;
        return true;
    }
    if (lua_type(lua, index) == LUA_TTABLE) {
        oa::FixedVec3 point{};
        if (!read_position(lua, index, "reachable", &point))
            return false;
        out->x = point.x;
        out->z = point.z;
        return true;
    }
    luaL_error(lua, "reachable expects a position or a unit");
    return false;
}

const char* field_text(lua_State* lua, int index, std::size_t* length) {
    if (lua_type(lua, index) != LUA_TSTRING)
        return nullptr;
    return lua_tolstring(lua, index, length);
}

int map_name(lua_State* lua, const Seat& seat) {
    if (seat.facts == nullptr) {
        lua_pushstring(lua, "");
        return 1;
    }
    lua_pushlstring(lua, seat.facts->map_name.data(), seat.facts->map_name.size());
    return 1;
}

int map_path(lua_State* lua, const Seat& seat) {
    if (seat.facts == nullptr || seat.facts->map_path.empty()) {
        lua_pushnil(lua);
        return 1;
    }
    lua_pushlstring(lua, seat.facts->map_path.data(), seat.facts->map_path.size());
    return 1;
}

extern "C" {

static int map_newindex(lua_State* lua) {
    return luaL_error(lua, "map is read-only");
}

static int method_wind(lua_State* lua) {
    if (!map_self(lua, "wind"))
        return 0;
    Seat* seat = live_seat(lua);
    if (seat == nullptr)
        return 0;
    const match_runtime::Match::WindState& wind = seat->match->wind();
    lua_pushinteger(lua, static_cast<lua_Integer>(wind.speed));
    lua_pushinteger(lua, static_cast<lua_Integer>(wind.direction));
    return 2;
}

static int method_height(lua_State* lua) {
    if (!map_self(lua, "height"))
        return 0;
    // The cell count and the corner height share one key. Called with no
    // cell, this is the height in cells. Called with a column and a row, it
    // is that cell's corner height. With no cell, a seat whose host has a
    // world reports that world's cell count and does not need a match.
    if (lua_gettop(lua) == 1) {
        Seat* seat = seat_of(lua);
        if (seat == nullptr)
            return 0;
        if (seat->calls.host.world != nullptr) {
            lua_pushinteger(lua, static_cast<lua_Integer>(seat->calls.host.map_cells_z));
            return 1;
        }
        if (seat->match == nullptr)
            return 0;
        lua_pushinteger(lua, static_cast<lua_Integer>(seat->match->state().game.map_height));
        return 1;
    }
    Seat* seat = live_seat(lua);
    if (seat == nullptr)
        return 0;
    int32_t x = 0;
    int32_t z = 0;
    if (!read_cell(lua, 2, "height", &x) || !read_cell(lua, 3, "height", &z))
        return 0;
    const oa::MapPlot* plot = oa::world_plot(&seat->match->state(), x, z);
    if (plot == nullptr) {
        lua_pushnil(lua);
        return 1;
    }
    lua_pushinteger(lua, static_cast<lua_Integer>(plot->height));
    return 1;
}

static int method_heights(lua_State* lua) {
    if (!map_self(lua, "heights"))
        return 0;
    int32_t x = 0;
    int32_t z = 0;
    if (!read_cell(lua, 2, "heights", &x) || !read_cell(lua, 3, "heights", &z))
        return 0;
    Seat* seat = live_seat(lua);
    if (seat == nullptr)
        return 0;
    const sim::spatial_state::Plot* plot = plot_at(*seat->match, x, z);
    if (plot == nullptr) {
        lua_pushnil(lua);
        return 1;
    }
    lua_pushinteger(lua, static_cast<lua_Integer>(plot->low_height));
    lua_pushinteger(lua, static_cast<lua_Integer>(plot->high_height));
    return 2;
}

static int method_ground(lua_State* lua) {
    if (!map_self(lua, "ground"))
        return 0;
    int32_t x = 0;
    int32_t z = 0;
    if (!read_fixed_arg(lua, 2, "ground", "x", &x) || !read_fixed_arg(lua, 3, "ground", "z", &z))
        return 0;
    Seat* seat = live_seat(lua);
    if (seat == nullptr)
        return 0;
    const int32_t height =
        seat->match->map_height(static_cast<uint32_t>(x), static_cast<uint32_t>(z));
    lua_pushinteger(lua, static_cast<lua_Integer>(height));
    return 1;
}

static int method_metal(lua_State* lua) {
    if (!map_self(lua, "metal"))
        return 0;
    int32_t x = 0;
    int32_t z = 0;
    if (!read_cell(lua, 2, "metal", &x) || !read_cell(lua, 3, "metal", &z))
        return 0;
    Seat* seat = seat_of(lua);
    if (seat == nullptr)
        return 0;
    if (seat->calls.host.cell_metal != nullptr) {
        uint8_t metal = 0;
        try {
            metal = seat->calls.host.cell_metal(seat->calls.host.context, x, z);
        } catch (...) {
            metal = 0;
        }
        lua_pushinteger(lua, static_cast<lua_Integer>(metal));
        return 1;
    }
    if (seat->match == nullptr)
        return 0;
    const sim::spatial_state::Plot* plot = plot_at(*seat->match, x, z);
    lua_pushinteger(lua, plot == nullptr ? lua_Integer{0} : static_cast<lua_Integer>(plot->metal));
    return 1;
}

static int method_metal_feature(lua_State* lua) {
    if (!map_self(lua, "metal_feature"))
        return 0;
    int32_t x = 0;
    int32_t z = 0;
    if (!read_cell(lua, 2, "metal_feature", &x) || !read_cell(lua, 3, "metal_feature", &z))
        return 0;
    Seat* seat = live_seat(lua);
    if (seat == nullptr)
        return 0;
    if (seat->calls.host.metal_feature != nullptr) {
        bool feature = false;
        try {
            feature = seat->calls.host.metal_feature(seat->calls.host.context, x, z);
        } catch (...) {
            feature = false;
        }
        lua_pushboolean(lua, feature ? 1 : 0);
        return 1;
    }
    const sim::spatial_state::Plot* plot = plot_at(*seat->match, x, z);
    lua_pushboolean(lua, plot != nullptr && metal_spot(*plot) ? 1 : 0);
    return 1;
}

int push_host_spots(lua_State* lua, const Seat& seat) {
    const auto& host = seat.calls.host;
    lua_newtable(lua);
    const int list = lua_gettop(lua);
    lua_Integer row = 1;
    uint64_t scanned = 0;
    for (int32_t z = 0; z < host.map_cells_z; ++z) {
        for (int32_t x = 0; x < host.map_cells_x; ++x) {
            ++scanned;
            bool feature = false;
            try {
                feature = host.metal_feature(host.context, x, z);
            } catch (...) {
                feature = false;
            }
            if (!feature)
                continue;
            lua_createtable(lua, 0, 2);
            lua_pushinteger(lua, static_cast<lua_Integer>(x));
            lua_setfield(lua, -2, "cx");
            lua_pushinteger(lua, static_cast<lua_Integer>(z));
            lua_setfield(lua, -2, "cz");
            lua_rawseti(lua, list, row);
            ++row;
        }
    }
    script_host::charge_work(lua, scanned * script_host::element_charge);
    return 1;
}

static int method_metal_spots(lua_State* lua) {
    if (!map_self(lua, "metal_spots"))
        return 0;
    Seat* seat = seat_of(lua);
    if (seat == nullptr)
        return 0;
    if (seat->match != nullptr)
        return push_spots(lua, *seat->match, metal_spot);
    if (seat->calls.host.metal_feature == nullptr || seat->calls.host.map_cells_x <= 0 ||
        seat->calls.host.map_cells_z <= 0)
        return 0;
    return push_host_spots(lua, *seat);
}

static int method_geo_spots(lua_State* lua) {
    if (!map_self(lua, "geo_spots"))
        return 0;
    Seat* seat = live_seat(lua);
    if (seat == nullptr)
        return 0;
    return push_spots(lua, *seat->match, geo_spot);
}

static int method_classes(lua_State* lua) {
    if (!map_self(lua, "classes"))
        return 0;
    Seat* seat = live_seat(lua);
    if (seat == nullptr)
        return 0;
    KnownClass known[most_classes]{};
    const oa::World& world = seat->match->state();
    uint32_t walked = 0;
    if (world.unit_defs != nullptr) {
        for (uint32_t type = 1; type < world.unit_def_count; ++type)
            ++walked;
    }
    const uint32_t count = collect_classes(*seat->match, known, most_classes);
    lua_createtable(lua, static_cast<int>(count), 0);
    lua_Integer row = 1;
    for (uint32_t i = 0; i < count; ++i) {
        const sim::ground_orders::MovementMap* grid =
            seat->match->movement_map(static_cast<sim::unit_spawn::AssetHandle>(known[i].handle));
        if (grid == nullptr)
            continue;
        const auto* definition = seat->match->unit_definition(known[i].type);
        lua_createtable(lua, 0, 4);
        if (definition == nullptr)
            lua_pushstring(lua, "");
        else
            lua_pushlstring(
                lua, definition->movement_class.data(), definition->movement_class.size()
            );
        lua_setfield(lua, -2, "name");
        lua_pushinteger(lua, static_cast<lua_Integer>(known[i].handle));
        lua_setfield(lua, -2, "handle");
        lua_pushinteger(lua, static_cast<lua_Integer>(grid->footprint_x()));
        lua_setfield(lua, -2, "footprint_x");
        lua_pushinteger(lua, static_cast<lua_Integer>(grid->footprint_z()));
        lua_setfield(lua, -2, "footprint_z");
        lua_rawseti(lua, -2, row);
        ++row;
    }
    script_host::charge_work(lua, static_cast<uint64_t>(walked) * script_host::element_charge);
    return 1;
}

static int method_passable(lua_State* lua) {
    if (!map_self(lua, "passable"))
        return 0;
    Seat* seat = live_seat(lua);
    if (seat == nullptr)
        return 0;
    char name[name_room];
    ClassUse used{};
    if (!resolve_class(lua, *seat, 2, name, sizeof name, &used))
        return 0;
    if (used.kind == ClassKind::aircraft)
        return refuse_class(lua, name) ? 0 : 0;
    int32_t x = 0;
    int32_t z = 0;
    if (!read_cell(lua, 3, "passable", &x) || !read_cell(lua, 4, "passable", &z))
        return 0;
    uint8_t value = 0;
    if (used.grid != nullptr && x >= 0 && z >= 0)
        value = used.grid->cell(static_cast<uint32_t>(x), static_cast<uint32_t>(z));
    lua_pushinteger(lua, static_cast<lua_Integer>(value));
    return 1;
}

static int method_anchor(lua_State* lua) {
    if (!map_self(lua, "anchor"))
        return 0;
    Seat* seat = live_seat(lua);
    if (seat == nullptr)
        return 0;
    char name[name_room];
    ClassUse used{};
    if (!resolve_class(lua, *seat, 2, name, sizeof name, &used))
        return 0;
    if (used.kind == ClassKind::aircraft)
        return refuse_class(lua, name) ? 0 : 0;
    oa::FixedVec3 point{};
    if (!read_position(lua, 3, "anchor", &point))
        return 0;
    int32_t cx = 0;
    int32_t cz = 0;
    anchor_cell(point.x, point.z, used.footprint_x, used.footprint_z, &cx, &cz);
    lua_pushinteger(lua, static_cast<lua_Integer>(cx));
    lua_pushinteger(lua, static_cast<lua_Integer>(cz));
    return 2;
}

static int method_reachable(lua_State* lua) {
    if (!map_self(lua, "reachable"))
        return 0;
    Seat* seat = live_seat(lua);
    if (seat == nullptr)
        return 0;
    Endpoint from{};
    Endpoint to{};
    if (!read_endpoint(lua, *seat, 2, &from) || !read_endpoint(lua, *seat, 3, &to))
        return 0;
    if (from.hidden || to.hidden) {
        script_host::charge_work(lua, reach_charge);
        lua_pushnil(lua);
        return 1;
    }
    char name[name_room];
    ClassUse used{};
    if (!resolve_class(lua, *seat, 4, name, sizeof name, &used))
        return 0;
    if (used.kind == ClassKind::aircraft) {
        script_host::charge_work(lua, reach_charge);
        lua_pushboolean(lua, 1);
        return 1;
    }
    uint64_t built = 0;
    if (used.grid != nullptr) {
        RegionBuild build;
        build.map = used.grid;
        build.handle = used.handle;
        build.tick = seat->match->state().game.tick;
        SightView sight;
        if (!seat->full_awareness) {
            const auto& grid = seat->match->sight();
            sight.grid = used.grid;
            sight.bits = grid.player_bits.data();
            sight.bit_count = grid.player_bits.size();
            sight.world_width = seat->match->spatial().terrain_width;
            sight.world_height = seat->match->spatial().terrain_height;
            sight.player = seat->player;
            build.context = &sight;
            build.pass = sight_pass;
        }
        built = ensure_regions(seat->regions, build);
    }
    int32_t ax = 0;
    int32_t az = 0;
    int32_t bx = 0;
    int32_t bz = 0;
    anchor_cell(from.x, from.z, used.footprint_x, used.footprint_z, &ax, &az);
    anchor_cell(to.x, to.z, used.footprint_x, used.footprint_z, &bx, &bz);
    const bool meets = regions_meet(seat->regions, used.handle, ax, az, bx, bz);
    script_host::charge_work(lua, built + reach_charge);
    lua_pushboolean(lua, meets ? 1 : 0);
    return 1;
}

static int map_index(lua_State* lua) {
    if (!map_self(lua, "index"))
        return 0;
    std::size_t length = 0;
    const char* key = field_text(lua, 2, &length);
    if (key == nullptr) {
        lua_pushnil(lua);
        return 1;
    }
    if (same_text(key, length, "wind")) {
        lua_pushcfunction(lua, method_wind);
        return 1;
    }
    if (same_text(key, length, "height")) {
        lua_pushcfunction(lua, method_height);
        return 1;
    }
    if (same_text(key, length, "heights")) {
        lua_pushcfunction(lua, method_heights);
        return 1;
    }
    if (same_text(key, length, "ground")) {
        lua_pushcfunction(lua, method_ground);
        return 1;
    }
    if (same_text(key, length, "metal")) {
        lua_pushcfunction(lua, method_metal);
        return 1;
    }
    if (same_text(key, length, "metal_feature")) {
        lua_pushcfunction(lua, method_metal_feature);
        return 1;
    }
    if (same_text(key, length, "metal_spots")) {
        lua_pushcfunction(lua, method_metal_spots);
        return 1;
    }
    if (same_text(key, length, "geo_spots")) {
        lua_pushcfunction(lua, method_geo_spots);
        return 1;
    }
    if (same_text(key, length, "classes")) {
        lua_pushcfunction(lua, method_classes);
        return 1;
    }
    if (same_text(key, length, "passable")) {
        lua_pushcfunction(lua, method_passable);
        return 1;
    }
    if (same_text(key, length, "anchor")) {
        lua_pushcfunction(lua, method_anchor);
        return 1;
    }
    if (same_text(key, length, "reachable")) {
        lua_pushcfunction(lua, method_reachable);
        return 1;
    }
    Seat* seat = seat_of(lua);
    if (seat == nullptr) {
        lua_pushnil(lua);
        return 1;
    }
    const oa::Game* game = nullptr;
    if (seat->match != nullptr)
        game = &seat->match->state().game;
    else if (seat->calls.host.world != nullptr)
        game = &seat->calls.host.world->game;
    if (game == nullptr) {
        lua_pushnil(lua);
        return 1;
    }
    if (same_text(key, length, "name"))
        return map_name(lua, *seat);
    if (same_text(key, length, "path"))
        return map_path(lua, *seat);
    if (same_text(key, length, "world")) {
        lua_pushnil(lua);
        return 1;
    }
    if (same_text(key, length, "width")) {
        const int32_t cells =
            seat->calls.host.world != nullptr ? seat->calls.host.map_cells_x : game->map_width;
        lua_pushinteger(lua, static_cast<lua_Integer>(cells));
        return 1;
    }
    if (same_text(key, length, "world_width")) {
        lua_pushinteger(lua, static_cast<lua_Integer>(game->map_width_world));
        return 1;
    }
    if (same_text(key, length, "world_height")) {
        lua_pushinteger(lua, static_cast<lua_Integer>(game->map_height_world));
        return 1;
    }
    if (same_text(key, length, "sea_level")) {
        lua_pushinteger(lua, static_cast<lua_Integer>(game->sea_level));
        return 1;
    }
    if (same_text(key, length, "tide")) {
        script_host::push_f32(lua, game->tidal_strength);
        return 1;
    }
    if (same_text(key, length, "wind_min")) {
        if (seat->match == nullptr) {
            lua_pushnil(lua);
            return 1;
        }
        lua_pushinteger(
            lua, static_cast<lua_Integer>(seat->match->environment_wind().minimum_strength)
        );
        return 1;
    }
    if (same_text(key, length, "wind_max")) {
        if (seat->match == nullptr) {
            lua_pushnil(lua);
            return 1;
        }
        lua_pushinteger(
            lua, static_cast<lua_Integer>(seat->match->environment_wind().maximum_strength)
        );
        return 1;
    }
    if (same_text(key, length, "surface_metal")) {
        int32_t metal = 0;
        if (seat->calls.host.world != nullptr)
            metal = seat->calls.host.surface_metal;
        else if (seat->match != nullptr)
            metal = surface_metal(*seat->match);
        else {
            lua_pushnil(lua);
            return 1;
        }
        lua_pushinteger(lua, static_cast<lua_Integer>(metal));
        return 1;
    }
    lua_pushnil(lua);
    return 1;
}

} // extern "C"

void ensure_meta(lua_State* lua) {
    if (luaL_newmetatable(lua, map_meta) == 0) {
        lua_pop(lua, 1);
        return;
    }
    lua_pushcfunction(lua, map_index);
    lua_setfield(lua, -2, "__index");
    lua_pushcfunction(lua, map_newindex);
    lua_setfield(lua, -2, "__newindex");
    lua_pushboolean(lua, 0);
    lua_setfield(lua, -2, "__metatable");
    lua_pop(lua, 1);
}

} // namespace

void install_map(lua_State* lua, Seat& seat) {
    if (seat.match == nullptr && seat.calls.host.world == nullptr) {
        lua_pushnil(lua);
        set_game_field(lua, seat, "map");
        return;
    }
    ensure_meta(lua);
    lua_newuserdatauv(lua, 1, 0);
    luaL_setmetatable(lua, map_meta);
    set_game_field(lua, seat, "map");
}

} // namespace oa::sim::brains
