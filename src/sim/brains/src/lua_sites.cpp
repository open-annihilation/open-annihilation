// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Building sites a seat may judge. A seat with full awareness uses the
// fog-free checks the built-in player uses. Every other seat is the placing
// player, so a site under fog is judged as that player would know it.
// The choice is the seat's full_awareness flag and nothing else.
#include "orders.hpp"

#include "definitions.hpp"
#include "handles_internal.hpp"

#include "oa/core/world.h"
#include "oa/data/script_host/fixed.hpp"
#include "oa/data/script_host/script_host.hpp"
#include "oa/sim/match_runtime.hpp"

#include "lua.hpp"

#include <cstdint>
#include <cstring>

namespace oa::sim::brains {
namespace {

constexpr uint64_t site_charge = 16;
constexpr int32_t max_place_cells = 4096;
constexpr int32_t max_place_ring = 4096;
constexpr int64_t cell_min = int64_t{-2147483647} - 1;
constexpr int64_t cell_max = 2147483647;

/// The footprint a facing turns, or false when the type is not in the table.
bool turned_footprint(
    const match_runtime::Match& match, uint16_t type, uint8_t facing, int32_t* across, int32_t* down
) noexcept {
    const auto& types = match.world().types;
    if (type == 0 || type >= types.size())
        return false;
    const bool turned = (match.build_facing(type, facing) & 1U) != 0;
    const auto& runtime = types[type];
    *across = static_cast<int32_t>(turned ? runtime.footprint_z : runtime.footprint_x);
    *down = static_cast<int32_t>(turned ? runtime.footprint_x : runtime.footprint_z);
    return true;
}

/// `site_clear_for`'s map-edge test: the facing's turned footprint stays on the map.
bool footprint_on_map(
    const match_runtime::Match& match, uint16_t type, int32_t cell_x, int32_t cell_z, uint8_t facing
) noexcept {
    int32_t across = 0;
    int32_t down = 0;
    if (!turned_footprint(match, type, facing, &across, &down))
        return false;
    const auto width = static_cast<int32_t>(match.spatial().terrain_width);
    const auto height = static_cast<int32_t>(match.spatial().terrain_height);
    return cell_x >= 0 && cell_z >= 0 && width > cell_x + across && height > cell_z + down;
}

bool is_building(const match_runtime::Match& match, uint16_t type) noexcept {
    const auto& types = match.world().types;
    return type != 0 && type < types.size() && types[type].bm_code == 0;
}

/// A label safe for a `%s` error. An empty name becomes "that type".
void building_label(
    const match_runtime::Match& match, uint16_t type, char* label, std::size_t room
) {
    if (room == 0)
        return;
    label[0] = '\0';
    const oa::World& world = match.state();
    if (world.unit_defs != nullptr && type < world.unit_def_count) {
        const char* name = world.unit_defs[type].unit_name;
        const std::size_t limit = room - 1 < 32 ? room - 1 : 32;
        std::size_t used = 0;
        while (used < limit && name[used] != '\0') {
            label[used] = name[used] == '%' ? '?' : name[used];
            ++used;
        }
        label[used] = '\0';
    }
    if (label[0] != '\0')
        return;
    const char* fallback = "that type";
    std::size_t used = 0;
    while (fallback[used] != '\0' && used + 1 < room) {
        label[used] = fallback[used];
        ++used;
    }
    label[used] = '\0';
}

/// The centre the built-in player gives an order: `(footprint + cell * 2) << 19`.
int32_t centre_axis(int32_t footprint, int32_t cell) noexcept {
    const int64_t sum = static_cast<int64_t>(footprint) + static_cast<int64_t>(cell) * 2;
    const auto bits = static_cast<uint32_t>(sum);
    return static_cast<int32_t>(bits << 19);
}

/// Floor division. C++ divides toward zero.
int64_t floor_div(int64_t number, int64_t divisor) noexcept {
    int64_t quot = number / divisor;
    const int64_t rest = number % divisor;
    if (rest != 0 && number < 0)
        --quot;
    return quot;
}

/// The footprint's top-left cell whose centre is `near`, so a centre round-trips.
int32_t anchor_cell(int32_t near, int32_t footprint) noexcept {
    const int64_t scaled = floor_div(static_cast<int64_t>(near), int64_t{1} << 19);
    const int64_t cell = floor_div(scaled - footprint, 2);
    if (cell > cell_max)
        return static_cast<int32_t>(cell_max);
    if (cell < cell_min)
        return static_cast<int32_t>(cell_min);
    return static_cast<int32_t>(cell);
}

bool add_cell(int32_t base, int32_t delta, int32_t* out) noexcept {
    const int64_t sum = static_cast<int64_t>(base) + delta;
    if (sum > cell_max || sum < cell_min)
        return false;
    *out = static_cast<int32_t>(sum);
    return true;
}

/// One site check. Exceptions from the match become a refusal, never a Lua error.
///
/// Facing 0 with full awareness uses the host's fog-free checks. Any other
/// facing uses `site_facing`. A seat under fog uses `site_fogged` after the
/// footprint test. A null pointer falls back to the match.
bool cell_clear(
    Seat& seat, uint16_t type, int32_t cell_x, int32_t cell_z, uint8_t facing, bool yard
) noexcept {
    match_runtime::Match& match = *seat.match;
    bool clear = false;
    try {
        if (seat.full_awareness && facing == 0) {
            if (yard && seat.calls.host.building_site != nullptr) {
                clear =
                    seat.calls.host.building_site(seat.calls.host.context, type, cell_x, cell_z);
            } else if (!yard && seat.calls.host.site_clear != nullptr) {
                clear = seat.calls.host.site_clear(seat.calls.host.context, type, cell_x, cell_z);
            } else if (yard) {
                match_runtime::BuildSiteOptions options;
                options.facing = facing;
                clear = match.building_site_clear(type, cell_x, cell_z, 0, options);
            } else {
                clear = match.site_clear_for(type, cell_x, cell_z, 0, 1, facing);
            }
        } else if (seat.full_awareness && seat.calls.site_facing != nullptr) {
            clear = seat.calls.site_facing(
                &seat.calls.host, type, cell_x, cell_z, static_cast<int32_t>(facing), yard
            );
        } else if (
            !seat.full_awareness && !yard && !footprint_on_map(match, type, cell_x, cell_z, facing)
        ) {
            clear = false;
        } else if (!seat.full_awareness && seat.calls.site_fogged != nullptr) {
            clear = seat.calls.site_fogged(
                &seat.calls.host,
                seat.player,
                type,
                cell_x,
                cell_z,
                static_cast<int32_t>(facing),
                yard
            );
        } else if (seat.full_awareness) {
            if (yard) {
                match_runtime::BuildSiteOptions options;
                options.facing = facing;
                clear = match.building_site_clear(type, cell_x, cell_z, 0, options);
            } else {
                clear = match.site_clear_for(type, cell_x, cell_z, 0, 1, facing);
            }
        } else {
            match_runtime::BuildSiteOptions options;
            options.facing = facing;
            clear = match.building_site(type, cell_x, cell_z, 0, seat.player, options).has_value();
        }
    } catch (...) {
        clear = false;
    }
    return clear;
}

bool read_cell(lua_State* state, int index, int32_t* cell) {
    if (lua_isinteger(state, index) == 0) {
        luaL_error(state, "site expects a cell");
        return false;
    }
    const lua_Integer value = lua_tointeger(state, index);
    if (value < cell_min || value > cell_max) {
        luaL_error(state, "site expects a cell");
        return false;
    }
    *cell = static_cast<int32_t>(value);
    return true;
}

bool read_site_facing(lua_State* state, int index, const char* method, uint8_t* facing) {
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

/// A mobile type on a seat without full awareness. Raised before any charge.
bool refuse_mobile(lua_State* state, const Seat& seat, uint16_t type) {
    if (seat.full_awareness || is_building(*seat.match, type))
        return false;
    char label[40];
    building_label(*seat.match, type, label, sizeof label);
    luaL_error(state, "site tests buildings; %s is not one", label);
    return true;
}

extern "C" {

int brain_site(lua_State* state) {
    Seat* seat = seat_of(state);
    if (seat == nullptr)
        return luaL_error(state, "site has no seat");
    // A null match is a load check. Return before any argument is read, so an
    // empty type table cannot raise here.
    if (seat->match == nullptr) {
        lua_pushnil(state);
        return 1;
    }
    uint16_t type = 0;
    if (!resolve_unit_type(state, 1, "site", &type))
        return 0;
    int32_t cell_x = 0;
    int32_t cell_z = 0;
    if (!read_cell(state, 2, &cell_x) || !read_cell(state, 3, &cell_z))
        return 0;
    uint8_t facing = 0;
    if (!read_site_facing(state, 4, "site", &facing))
        return 0;
    bool yard = false;
    if (!lua_isnoneornil(state, 5)) {
        const char* word = lua_type(state, 5) == LUA_TSTRING ? lua_tostring(state, 5) : nullptr;
        if (word != nullptr && std::strcmp(word, "site") == 0)
            yard = false;
        else if (word != nullptr && std::strcmp(word, "yard") == 0)
            yard = true;
        else
            return luaL_error(state, "site expects \"site\" or \"yard\"");
    }
    if (refuse_mobile(state, *seat, type))
        return 0;
    script_host::charge_work(state, site_charge);
    const bool clear = cell_clear(*seat, type, cell_x, cell_z, facing, yard);
    lua_pushboolean(state, clear ? 1 : 0);
    return 1;
}

int brain_place(lua_State* state) {
    Seat* seat = seat_of(state);
    if (seat == nullptr)
        return luaL_error(state, "place has no seat");
    if (seat->match == nullptr) {
        lua_pushnil(state);
        return 1;
    }
    uint16_t type = 0;
    if (!resolve_unit_type(state, 1, "place", &type))
        return 0;
    oa::FixedVec3 near{};
    if (!read_position(state, 2, "place", &near))
        return 0;
    if (lua_isinteger(state, 3) == 0)
        return luaL_error(state, "place expects a radius in world units");
    const lua_Integer radius = lua_tointeger(state, 3);
    if (radius < 0)
        return luaL_error(state, "place expects a radius in world units");
    uint8_t facing = 0;
    if (!read_site_facing(state, 4, "place", &facing))
        return 0;
    if (refuse_mobile(state, *seat, type))
        return 0;
    int32_t across = 0;
    int32_t down = 0;
    if (!turned_footprint(*seat->match, type, facing, &across, &down)) {
        lua_pushnil(state);
        return 1;
    }
    const int32_t anchor_x = anchor_cell(near.x, across);
    const int32_t anchor_z = anchor_cell(near.z, down);
    int32_t ring_limit = max_place_ring;
    const int64_t rings = radius / 16;
    if (rings < ring_limit)
        ring_limit = static_cast<int32_t>(rings);

    int32_t tried = 0;
    bool found = false;
    const auto consider = [&](int32_t cell_x, int32_t cell_z) -> bool {
        if (tried >= max_place_cells)
            return true;
        ++tried;
        script_host::charge_work(state, site_charge);
        const bool clear = cell_clear(*seat, type, cell_x, cell_z, facing, false);
        if (!clear)
            return false;
        lua_pushinteger(state, cell_x);
        lua_pushinteger(state, cell_z);
        script_host::push_fixed(state, static_cast<int64_t>(centre_axis(across, cell_x)));
        script_host::push_fixed(state, static_cast<int64_t>(centre_axis(down, cell_z)));
        found = true;
        return true;
    };

    for (int32_t ring = 0; ring <= ring_limit && !found; ++ring) {
        int32_t west = 0;
        int32_t east = 0;
        int32_t north = 0;
        int32_t south = 0;
        if (!add_cell(anchor_x, -ring, &west) || !add_cell(anchor_x, ring, &east) ||
            !add_cell(anchor_z, -ring, &north) || !add_cell(anchor_z, ring, &south))
            break;
        if (ring == 0) {
            if (consider(anchor_x, anchor_z))
                break;
            continue;
        }
        bool stop = false;
        for (int32_t cell_x = west; cell_x <= east; ++cell_x) {
            if (consider(cell_x, north)) {
                stop = true;
                break;
            }
        }
        if (stop)
            break;
        for (int32_t cell_z = north + 1; cell_z <= south; ++cell_z) {
            if (consider(east, cell_z)) {
                stop = true;
                break;
            }
        }
        if (stop)
            break;
        for (int32_t cell_x = east - 1; cell_x >= west; --cell_x) {
            if (consider(cell_x, south)) {
                stop = true;
                break;
            }
        }
        if (stop)
            break;
        for (int32_t cell_z = south - 1; cell_z >= north + 1; --cell_z) {
            if (consider(west, cell_z)) {
                stop = true;
                break;
            }
        }
    }
    if (found)
        return 4;
    lua_pushnil(state);
    return 1;
}

} // extern "C"

} // namespace

void install_site_functions(lua_State* state, Seat&) {
    lua_getglobal(state, "brain");
    if (lua_type(state, -1) != LUA_TTABLE) {
        lua_pop(state, 1);
        lua_newtable(state);
    }
    lua_pushcfunction(state, brain_site);
    lua_setfield(state, -2, "site");
    lua_pushcfunction(state, brain_place);
    lua_setfield(state, -2, "place");
    lua_setglobal(state, "brain");
}

} // namespace oa::sim::brains
