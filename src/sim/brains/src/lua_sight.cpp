// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Sight, radar and the full-awareness search. Nothing here reads the seat's
// match when it is null, and nothing reads the viewpoint's radar bits.

#include "sight.hpp"

#include "handles_internal.hpp"
#include "internal.hpp"
#include "lua_game.hpp"

#include "oa/core/player.h"
#include "oa/core/unit.h"
#include "oa/core/world.h"
#include "oa/data/script_host/fixed.hpp"
#include "oa/data/script_host/script_host.hpp"
#include "oa/sim/detection.hpp"
#include "oa/sim/match_runtime.hpp"
#include "oa/sim/simulation_state.hpp"
#include "oa/sim/visibility_state.hpp"

#include "lua.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <new>
#include <span>
#include <utility>

namespace oa::sim::brains {
namespace {

char contact_cache_token;

constexpr int64_t component_min = int64_t{-2147483647} - 1;
constexpr int64_t component_max = 2147483647;
constexpr lua_Integer whole_min = -32768;
constexpr lua_Integer whole_max = 32767;
constexpr uint64_t geometry_charge = 4;
constexpr uint64_t search_charge = 64;
constexpr int32_t sight_cell_size = 32;
constexpr uint8_t grid_sight = 0;
constexpr uint8_t grid_explored = 1;
// The definition collection already owns oa.brain.units.
constexpr const char* world_units_meta = "oa.brain.world_units";

struct ContactBody {
    uint16_t slot{};
    uint32_t generation{};
    uint8_t sonar{};
    uint8_t radar{};
};

struct GridBody {
    uint8_t kind{};
};

struct UnitsBody {
    uint8_t unused{};
};

void push_nil(lua_State* lua) {
    lua_pushnil(lua);
}

void push_bool(lua_State* lua, bool value) {
    lua_pushboolean(lua, value ? 1 : 0);
}

void charge_one(lua_State* lua) {
    script_host::charge_work(lua, script_host::element_charge);
}

void charge_sequence(lua_State* lua, std::size_t elements) {
    script_host::charge_work(
        lua, script_host::element_charge * (static_cast<uint64_t>(elements) + 1)
    );
}

int16_t low_half(uint32_t word) noexcept {
    return std::bit_cast<int16_t>(static_cast<uint16_t>(word));
}

bool names(lua_State* lua, int index, const char* text) {
    if (text == nullptr || lua_type(lua, index) != LUA_TSTRING)
        return false;
    const char* held = lua_tostring(lua, index);
    return held != nullptr && std::strcmp(held, text) == 0;
}

bool read_plane(lua_State* lua, int index, const char* message, int32_t* out) {
    int64_t count = 0;
    if (script_host::to_fixed(lua, index, &count)) {
        if (count < component_min || count > component_max) {
            luaL_error(lua, "%s", message);
            return false;
        }
        *out = static_cast<int32_t>(count);
        return true;
    }
    if (lua_isinteger(lua, index) != 0) {
        const lua_Integer whole = lua_tointeger(lua, index);
        if (whole < whole_min || whole > whole_max) {
            luaL_error(lua, "%s", message);
            return false;
        }
        *out = static_cast<int32_t>(static_cast<int64_t>(whole) * int64_t{65536});
        return true;
    }
    luaL_error(lua, "%s", message);
    return false;
}

bool read_cell(lua_State* lua, int index, int32_t* cell) {
    if (lua_isinteger(lua, index) == 0) {
        luaL_error(lua, "at expects cell coordinates");
        return false;
    }
    const lua_Integer value = lua_tointeger(lua, index);
    if (value < whole_min || value > whole_max) {
        luaL_error(lua, "at expects cell coordinates");
        return false;
    }
    *cell = static_cast<int32_t>(value);
    return true;
}

const sim::detection::Sightings* seen_list(const Seat& seat) noexcept {
    if (seat.match == nullptr || seat.player >= OA_PLAYER_COUNT)
        return nullptr;
    try {
        return &seat.match->sightings(seat.player);
    } catch (...) {
        return nullptr;
    }
}

bool seat_sees(const Seat& seat, UnitRef ref) noexcept {
    if (seat.full_awareness)
        return true;
    if (seat.match == nullptr)
        return false;
    try {
        return seat.match->unit_visible(seat.player, ref.slot);
    } catch (...) {
        return false;
    }
}

bool enemy_contact_at(
    const Seat& seat, std::span<const uint8_t> bits, uint32_t slot, ContactEntry* out
) noexcept {
    if (seat.match == nullptr || slot >= bits.size() || slot > 65535u)
        return false;
    const uint8_t picture = bits[slot];
    if ((picture & (sim::detection::picture_radar | sim::detection::picture_sonar)) == 0)
        return false;
    const oa::World& world = seat.match->state();
    if (slot >= world.unit_slot_count)
        return false;
    const oa::Unit& unit = world.units[slot];
    if (unit.type_index == 0 || !oa::unit_is_live_target(unit.flags))
        return false;
    if (unit.owner_index >= OA_PLAYER_COUNT)
        return false;
    if (seat.match->allied(seat.player, unit.owner_index))
        return false;
    if (out != nullptr) {
        out->unit = static_cast<uint16_t>(slot);
        out->generation = unit_generation(*seat.match, static_cast<uint16_t>(slot));
        out->sonar = (picture & sim::detection::picture_sonar) != 0;
        out->radar = (picture & sim::detection::picture_radar) != 0;
    }
    return true;
}

uint32_t write_enemy_contacts(
    const Seat& seat, std::span<const uint8_t> bits, ContactEntry* dest, uint32_t room
) noexcept {
    uint32_t count = 0;
    const uint32_t span = static_cast<uint32_t>(bits.size());
    for (uint32_t slot = 0; slot < span; ++slot) {
        ContactEntry entry;
        if (!enemy_contact_at(seat, bits, slot, &entry))
            continue;
        if (dest != nullptr && count < room)
            dest[count] = entry;
        ++count;
    }
    if (count > room)
        return room;
    return count;
}

bool contact_still(const Seat& seat, uint16_t slot, uint32_t generation) noexcept {
    if (seat.match == nullptr)
        return false;
    const auto bits = seat.match->player_contact_bits(seat.player);
    ContactEntry entry;
    if (!enemy_contact_at(seat, bits, slot, &entry))
        return false;
    return entry.generation == generation;
}

const oa::Unit* contact_unit(const Seat& seat, uint16_t slot) noexcept {
    if (seat.match == nullptr)
        return nullptr;
    return oa::world_unit_at(&seat.match->state(), slot);
}

void ensure_cache(lua_State* lua) {
    lua_rawgetp(lua, LUA_REGISTRYINDEX, &contact_cache_token);
    if (lua_type(lua, -1) == LUA_TTABLE)
        return;
    lua_pop(lua, 1);
    lua_newtable(lua);
    lua_pushvalue(lua, -1);
    lua_rawsetp(lua, LUA_REGISTRYINDEX, &contact_cache_token);
}

const ContactBody* contact_body(lua_State* lua) {
    if (lua_type(lua, 1) != LUA_TUSERDATA)
        return nullptr;
    if (lua_getmetatable(lua, 1) == 0)
        return nullptr;
    luaL_getmetatable(lua, "oa.brain.contact");
    const bool same = lua_rawequal(lua, -1, -2) != 0;
    lua_pop(lua, 2);
    if (!same)
        return nullptr;
    return static_cast<const ContactBody*>(lua_touserdata(lua, 1));
}

const GridBody* grid_body(lua_State* lua) {
    if (lua_type(lua, 1) != LUA_TUSERDATA)
        return nullptr;
    if (lua_getmetatable(lua, 1) == 0)
        return nullptr;
    luaL_getmetatable(lua, "oa.brain.grid");
    const bool same = lua_rawequal(lua, -1, -2) != 0;
    lua_pop(lua, 2);
    if (!same)
        return nullptr;
    return static_cast<const GridBody*>(lua_touserdata(lua, 1));
}

bool cell_on_grid(const sim::visibility_state::PlayerSightGrid& grid, int32_t cx, int32_t cz) {
    return cx >= 0 && cz >= 0 && static_cast<uint32_t>(cx) < static_cast<uint32_t>(grid.width) &&
           static_cast<uint32_t>(cz) < static_cast<uint32_t>(grid.height);
}

std::size_t cell_index(const sim::visibility_state::PlayerSightGrid& grid, int32_t cx, int32_t cz) {
    return static_cast<std::size_t>(cz) * static_cast<std::size_t>(grid.width) +
           static_cast<std::size_t>(cx);
}

extern "C" {

int contact_index(lua_State* lua) {
    if (lua_type(lua, 2) != LUA_TSTRING)
        return 0;
    const ContactBody* body = contact_body(lua);
    if (body == nullptr)
        return 0;
    const Seat* seat = seat_of(lua);
    const bool live = seat != nullptr && contact_still(*seat, body->slot, body->generation);
    const oa::Unit* unit = live ? contact_unit(*seat, body->slot) : nullptr;
    if (names(lua, 2, "sonar")) {
        push_bool(lua, body->sonar != 0);
        return 1;
    }
    if (names(lua, 2, "radar")) {
        push_bool(lua, body->radar != 0);
        return 1;
    }
    if (names(lua, 2, "current")) {
        push_bool(lua, live);
        return 1;
    }
    if (names(lua, 2, "x") || names(lua, 2, "y") || names(lua, 2, "z")) {
        if (unit == nullptr) {
            push_nil(lua);
            return 1;
        }
        int32_t component = unit->position.x;
        if (names(lua, 2, "y"))
            component = unit->position.y;
        else if (names(lua, 2, "z"))
            component = unit->position.z;
        script_host::push_fixed(lua, component);
        return 1;
    }
    if (names(lua, 2, "position")) {
        if (unit == nullptr) {
            push_nil(lua);
            return 1;
        }
        push_position(lua, unit->position);
        return 1;
    }
    return 0;
}

int contact_newindex(lua_State* lua) {
    return luaL_error(lua, "a contact cannot be assigned");
}

int grid_at(lua_State* lua) {
    int32_t cx = 0;
    int32_t cz = 0;
    if (!read_cell(lua, 2, &cx) || !read_cell(lua, 3, &cz))
        return 0;
    charge_one(lua);
    const GridBody* body = grid_body(lua);
    const Seat* seat = seat_of(lua);
    bool on = false;
    if (body != nullptr && seat != nullptr && seat->match != nullptr) {
        const sim::visibility_state::PlayerSightGrid& grid = seat->match->sight();
        if (cell_on_grid(grid, cx, cz)) {
            const std::size_t cell = cell_index(grid, cx, cz);
            if (body->kind == grid_sight) {
                const auto coverage = seat->match->player_coverage(seat->player);
                if (cell < coverage.size()) {
                    on = sim::visibility_state::count_in_sight(
                        coverage[cell], sim::visibility_state::SightReading::simulation
                    );
                }
            } else if (cell < grid.player_bits.size() && seat->player < OA_PLAYER_COUNT) {
                const auto mask = static_cast<uint16_t>(uint16_t{1} << seat->player);
                on = (grid.player_bits[cell] & mask) != 0;
            }
        }
    }
    push_bool(lua, on);
    return 1;
}

int grid_cell_of(lua_State* lua) {
    constexpr const char* message = "cell_of expects x and z as fixed values or whole numbers";
    int32_t x = 0;
    int32_t z = 0;
    if (!read_plane(lua, 2, message, &x) || !read_plane(lua, 3, message, &z))
        return 0;
    charge_one(lua);
    const Seat* seat = seat_of(lua);
    if (seat == nullptr || seat->match == nullptr) {
        push_nil(lua);
        return 1;
    }
    const sim::visibility_state::PlayerSightGrid& grid = seat->match->sight();
    const auto cx = static_cast<int32_t>(low_half(static_cast<uint32_t>(x) >> 16)) >> 5;
    const auto cz = static_cast<int32_t>(low_half(static_cast<uint32_t>(z) >> 16)) >> 5;
    if (!cell_on_grid(grid, cx, cz)) {
        push_nil(lua);
        return 1;
    }
    lua_pushinteger(lua, static_cast<lua_Integer>(cx));
    lua_pushinteger(lua, static_cast<lua_Integer>(cz));
    return 2;
}

int grid_index(lua_State* lua) {
    if (lua_type(lua, 2) != LUA_TSTRING)
        return 0;
    if (names(lua, 2, "at")) {
        lua_pushcfunction(lua, grid_at);
        return 1;
    }
    if (names(lua, 2, "cell_of")) {
        lua_pushcfunction(lua, grid_cell_of);
        return 1;
    }
    if (names(lua, 2, "cell_size")) {
        charge_one(lua);
        lua_pushinteger(lua, sight_cell_size);
        return 1;
    }
    const Seat* seat = seat_of(lua);
    int32_t width = 0;
    int32_t height = 0;
    if (seat != nullptr && seat->match != nullptr) {
        width = seat->match->sight().width;
        height = seat->match->sight().height;
    }
    if (names(lua, 2, "width")) {
        charge_one(lua);
        lua_pushinteger(lua, static_cast<lua_Integer>(width));
        return 1;
    }
    if (names(lua, 2, "height")) {
        charge_one(lua);
        lua_pushinteger(lua, static_cast<lua_Integer>(height));
        return 1;
    }
    return 0;
}

int grid_newindex(lua_State* lua) {
    return luaL_error(lua, "a sight grid cannot be assigned");
}

int units_each(lua_State* lua) {
    if (lua_type(lua, 2) != LUA_TFUNCTION)
        return luaL_error(lua, "each expects a function");
    Seat* seat = seat_of(lua);
    std::unique_ptr<UnitRef[]> held;
    uint32_t found = 0;
    if (seat != nullptr && seat->match != nullptr) {
        const oa::World& world = seat->match->state();
        const uint32_t slots = world.unit_slot_count;
        if (slots > 0) {
            try {
                held = std::make_unique<UnitRef[]>(slots);
            } catch (const std::bad_alloc&) {
                return luaL_error(lua, "out of memory");
            }
            for (uint32_t slot = 0; slot < slots; ++slot) {
                if (slot > 65535u || world.units[slot].type_index == 0)
                    continue;
                held[found] = unit_ref(*seat->match, static_cast<uint16_t>(slot));
                ++found;
            }
        }
    }
    charge_sequence(lua, found);
    for (uint32_t row = 0; row < found; ++row) {
        lua_pushvalue(lua, 2);
        push_unit(lua, *seat, held[row]);
        lua_call(lua, 1, 0);
    }
    return 0;
}

int units_list(lua_State* lua) {
    int player_filter = -1;
    if (lua_gettop(lua) >= 2 && lua_type(lua, 2) != LUA_TNIL) {
        if (lua_isinteger(lua, 2) == 0)
            return luaL_error(lua, "list expects a player from 0 to 9");
        const lua_Integer which = lua_tointeger(lua, 2);
        if (which < 0 || which > 9)
            return luaL_error(lua, "list expects a player from 0 to 9");
        player_filter = static_cast<int>(which);
    }
    Seat* seat = seat_of(lua);
    const oa::World* world = nullptr;
    uint32_t slots = 0;
    if (seat != nullptr && seat->match != nullptr) {
        world = &seat->match->state();
        slots = world->unit_slot_count;
    }
    uint32_t found = 0;
    for (uint32_t slot = 0; slot < slots; ++slot) {
        if (slot > 65535u)
            continue;
        const oa::Unit& unit = world->units[slot];
        if (unit.type_index == 0)
            continue;
        if (player_filter >= 0 && unit.owner_index != static_cast<uint8_t>(player_filter))
            continue;
        ++found;
    }
    charge_sequence(lua, found);
    const int hint = found > 1000000u ? 0 : static_cast<int>(found);
    lua_createtable(lua, hint, 0);
    if (world == nullptr || seat == nullptr || found == 0)
        return 1;
    const int sequence = lua_gettop(lua);
    lua_Integer row = 0;
    for (uint32_t slot = 0; slot < slots; ++slot) {
        if (slot > 65535u)
            continue;
        const oa::Unit& unit = world->units[slot];
        if (unit.type_index == 0)
            continue;
        if (player_filter >= 0 && unit.owner_index != static_cast<uint8_t>(player_filter))
            continue;
        push_unit(lua, *seat, unit_ref(*seat->match, static_cast<uint16_t>(slot)));
        lua_rawseti(lua, sequence, ++row);
    }
    return 1;
}

int units_index(lua_State* lua) {
    if (names(lua, 2, "each")) {
        lua_pushcfunction(lua, units_each);
        return 1;
    }
    if (names(lua, 2, "list")) {
        lua_pushcfunction(lua, units_list);
        return 1;
    }
    return 0;
}

int units_newindex(lua_State* lua) {
    return luaL_error(lua, "units cannot be assigned");
}

int getter_full_awareness(lua_State* lua) {
    charge_one(lua);
    const Seat* seat = seat_of(lua);
    push_bool(lua, seat != nullptr && seat->full_awareness);
    return 1;
}

int getter_seen(lua_State* lua) {
    Seat* seat = seat_of(lua);
    const uint16_t* slots = nullptr;
    uint32_t count = 0;
    if (seat != nullptr) {
        const sim::detection::Sightings* sight = seen_list(*seat);
        if (sight != nullptr && sight->seen != nullptr) {
            slots = sight->seen;
            count = sight->seen_count;
        }
    }
    charge_sequence(lua, count);
    const int hint = count > 1000000u ? 0 : static_cast<int>(count);
    lua_createtable(lua, hint, 0);
    if (slots == nullptr || seat == nullptr || seat->match == nullptr || count == 0)
        return 1;
    const int sequence = lua_gettop(lua);
    for (uint32_t index = 0; index < count; ++index) {
        push_unit(lua, *seat, unit_ref(*seat->match, slots[index]));
        lua_rawseti(lua, sequence, static_cast<lua_Integer>(index + 1));
    }
    return 1;
}

int getter_seen_tick(lua_State* lua) {
    charge_one(lua);
    const Seat* seat = seat_of(lua);
    if (seat == nullptr) {
        push_nil(lua);
        return 1;
    }
    const sim::detection::Sightings* sight = seen_list(*seat);
    if (sight == nullptr) {
        push_nil(lua);
        return 1;
    }
    lua_pushinteger(lua, static_cast<lua_Integer>(sight->refreshed_tick));
    return 1;
}

int getter_radar(lua_State* lua) {
    Seat* seat = seat_of(lua);
    std::span<const uint8_t> bits;
    if (seat != nullptr && seat->match != nullptr)
        bits = seat->match->player_contact_bits(seat->player);
    uint32_t found = 0;
    if (seat != nullptr)
        found = write_enemy_contacts(*seat, bits, nullptr, 0xffffffffu);
    charge_sequence(lua, found);
    const int hint = found > 1000000u ? 0 : static_cast<int>(found);
    lua_createtable(lua, hint, 0);
    if (seat == nullptr || found == 0)
        return 1;
    const int sequence = lua_gettop(lua);
    lua_Integer row = 0;
    const uint32_t span = static_cast<uint32_t>(bits.size());
    for (uint32_t slot = 0; slot < span && static_cast<uint32_t>(row) < found; ++slot) {
        ContactEntry entry;
        if (!enemy_contact_at(*seat, bits, slot, &entry))
            continue;
        push_contact(lua, *seat, entry);
        lua_rawseti(lua, sequence, ++row);
    }
    return 1;
}

int getter_radar_tick(lua_State* lua) {
    charge_one(lua);
    const Seat* seat = seat_of(lua);
    if (seat == nullptr || seat->match == nullptr) {
        push_nil(lua);
        return 1;
    }
    lua_pushinteger(lua, static_cast<lua_Integer>(seat->match->player_contacts_tick(seat->player)));
    return 1;
}

int method_sees(lua_State* lua) {
    constexpr const char* message = "sees expects x and z as fixed values or whole numbers";
    int32_t x = 0;
    int32_t z = 0;
    int32_t y = 0;
    if (!read_plane(lua, 2, message, &x) || !read_plane(lua, 3, message, &z))
        return 0;
    if (lua_gettop(lua) >= 4 && lua_type(lua, 4) != LUA_TNIL) {
        if (!read_plane(lua, 4, "sees expects y as a fixed value or a whole number", &y))
            return 0;
    }
    script_host::charge_work(lua, geometry_charge);
    const Seat* seat = seat_of(lua);
    bool visible = false;
    if (seat != nullptr && seat->match != nullptr) {
        const std::array<uint32_t, 3> point = {
            static_cast<uint32_t>(x), static_cast<uint32_t>(y), static_cast<uint32_t>(z)
        };
        try {
            visible = seat->match->point_visible(seat->player, point);
        } catch (...) {
            visible = false;
        }
    }
    push_bool(lua, visible);
    return 1;
}

int method_sees_unit(lua_State* lua) {
    UnitRef ref{};
    if (check_unit(lua, 2, "sees_unit", &ref) != 0)
        return 0;
    script_host::charge_work(lua, geometry_charge);
    const Seat* seat = seat_of(lua);
    bool visible = false;
    if (seat != nullptr && seat->match != nullptr && unit_current(*seat->match, ref)) {
        try {
            visible = seat->match->unit_visible(seat->player, ref.slot);
        } catch (...) {
            visible = false;
        }
    }
    push_bool(lua, visible);
    return 1;
}

int push_grid(lua_State* lua, uint8_t kind) {
    charge_one(lua);
    const Seat* seat = seat_of(lua);
    if (seat == nullptr || seat->match == nullptr || seat->match->sight().width == 0) {
        push_nil(lua);
        return 1;
    }
    auto* body = static_cast<GridBody*>(lua_newuserdatauv(lua, sizeof(GridBody), 0));
    body->kind = kind;
    luaL_getmetatable(lua, "oa.brain.grid");
    if (lua_type(lua, -1) == LUA_TTABLE)
        lua_setmetatable(lua, -2);
    else
        lua_pop(lua, 1);
    return 1;
}

int getter_sight(lua_State* lua) {
    return push_grid(lua, grid_sight);
}

int getter_explored(lua_State* lua) {
    return push_grid(lua, grid_explored);
}

int method_nearest_enemy(lua_State* lua) {
    constexpr const char* message =
        "nearest_enemy expects x and z as fixed values or whole numbers";
    int32_t x = 0;
    int32_t z = 0;
    if (!read_plane(lua, 2, message, &x) || !read_plane(lua, 3, message, &z))
        return 0;
    sim::simulation_state::CandidateFilter filter{};
    if (lua_gettop(lua) >= 4 && lua_type(lua, 4) != LUA_TNIL) {
        if (lua_type(lua, 4) != LUA_TTABLE)
            return luaL_error(lua, "nearest_enemy expects a filter table");
        lua_getfield(lua, 4, "widened");
        filter.widened_flags = lua_toboolean(lua, -1) != 0;
        lua_pop(lua, 1);
        lua_getfield(lua, 4, "skip_submerged");
        filter.skip_submerged = lua_toboolean(lua, -1) != 0;
        lua_pop(lua, 1);
    }
    script_host::charge_work(lua, search_charge);
    Seat* seat = seat_of(lua);
    if (seat == nullptr || seat->match == nullptr) {
        push_nil(lua);
        return 1;
    }
    uint8_t relation[OA_PLAYER_COUNT + 1]{};
    for (uint8_t player = 0; player < OA_PLAYER_COUNT; ++player) {
        const bool allied = seat->match->allied(seat->player, player);
        relation[player] = allied ? static_cast<uint8_t>(1) : static_cast<uint8_t>(0);
    }
    relation[OA_PLAYER_COUNT] = static_cast<uint8_t>(1);
    oa::Unit* found =
        sim::simulation_state::nearest_candidate_unit(seat->match->state(), relation, x, z, filter);
    if (found == nullptr) {
        push_nil(lua);
        return 1;
    }
    const uint32_t slot = oa::world_unit_slot(&seat->match->state(), found);
    if (slot > 65535u) {
        push_nil(lua);
        return 1;
    }
    push_unit(lua, *seat, unit_ref(*seat->match, static_cast<uint16_t>(slot)));
    return 1;
}

int method_seen_raw(lua_State* lua) {
    Seat* seat = seat_of(lua);
    const uint16_t* slots = nullptr;
    uint32_t count = 0;
    if (seat != nullptr) {
        const sim::detection::Sightings* sight = seen_list(*seat);
        if (sight != nullptr && sight->seen != nullptr) {
            slots = sight->seen;
            count = sight->seen_count;
        }
    }
    charge_sequence(lua, count);
    const int hint = count > 1000000u ? 0 : static_cast<int>(count);
    lua_createtable(lua, hint, 0);
    if (slots == nullptr || seat == nullptr || seat->match == nullptr || count == 0)
        return 1;
    const int sequence = lua_gettop(lua);
    const oa::World& world = seat->match->state();
    for (uint32_t index = 0; index < count; ++index) {
        const uint16_t slot = slots[index];
        const oa::Unit* unit = oa::world_unit_at(&world, slot);
        lua_createtable(lua, 0, 5);
        lua_pushinteger(lua, static_cast<lua_Integer>(slot));
        lua_setfield(lua, -2, "slot");
        const uint16_t type_index = unit != nullptr ? unit->type_index : 0;
        lua_pushinteger(lua, static_cast<lua_Integer>(type_index));
        lua_setfield(lua, -2, "type_index");
        const int32_t x = unit != nullptr ? unit->position.x : 0;
        const int32_t y = unit != nullptr ? unit->position.y : 0;
        const int32_t z = unit != nullptr ? unit->position.z : 0;
        script_host::push_fixed(lua, x);
        lua_setfield(lua, -2, "x");
        script_host::push_fixed(lua, y);
        lua_setfield(lua, -2, "y");
        script_host::push_fixed(lua, z);
        lua_setfield(lua, -2, "z");
        lua_rawseti(lua, sequence, static_cast<lua_Integer>(index + 1));
    }
    return 1;
}

} // extern "C"

void install_type(lua_State* lua, const char* name, lua_CFunction index, lua_CFunction assign) {
    if (luaL_newmetatable(lua, name) == 0) {
        lua_pop(lua, 1);
        return;
    }
    lua_pushcfunction(lua, index);
    lua_setfield(lua, -2, "__index");
    lua_pushcfunction(lua, assign);
    lua_setfield(lua, -2, "__newindex");
    lua_pushboolean(lua, 0);
    lua_setfield(lua, -2, "__metatable");
    lua_pop(lua, 1);
}

} // namespace

uint32_t collect_enemy_contacts(lua_State* lua, Seat& seat) {
    if (seat.match == nullptr)
        return 0;
    const auto bits = seat.match->player_contact_bits(seat.player);
    const auto span = static_cast<uint32_t>(bits.size());
    if (seat.contact_capacity == 0 && span > 0) {
        try {
            auto delivered = std::make_unique<ContactEntry[]>(span);
            auto work = std::make_unique<ContactEntry[]>(span);
            seat.delivered_contacts = std::move(delivered);
            seat.contact_work = std::move(work);
        } catch (const std::bad_alloc&) {
            luaL_error(lua, "out of memory");
            return 0;
        }
        seat.contact_capacity = span;
    }
    const uint32_t room = span < seat.contact_capacity ? span : seat.contact_capacity;
    script_host::charge_work(lua, script_host::element_charge * static_cast<uint64_t>(room));
    if (seat.contact_work == nullptr || room == 0 || bits.data() == nullptr)
        return 0;
    const std::span<const uint8_t> used{bits.data(), static_cast<std::size_t>(room)};
    return write_enemy_contacts(seat, used, seat.contact_work.get(), room);
}

void push_contact(lua_State* lua, Seat& seat, const ContactEntry& entry) {
    (void)seat;
    ensure_cache(lua);
    const int cache = lua_gettop(lua);
    lua_rawgeti(lua, cache, static_cast<lua_Integer>(entry.unit));
    if (lua_type(lua, -1) == LUA_TUSERDATA) {
        auto* body = static_cast<ContactBody*>(lua_touserdata(lua, -1));
        if (body != nullptr && body->slot == entry.unit && body->generation == entry.generation) {
            body->sonar = entry.sonar ? static_cast<uint8_t>(1) : static_cast<uint8_t>(0);
            body->radar = entry.radar ? static_cast<uint8_t>(1) : static_cast<uint8_t>(0);
            lua_remove(lua, cache);
            return;
        }
    }
    lua_pop(lua, 1);
    auto* created = static_cast<ContactBody*>(lua_newuserdatauv(lua, sizeof(ContactBody), 0));
    created->slot = entry.unit;
    created->generation = entry.generation;
    created->sonar = entry.sonar ? static_cast<uint8_t>(1) : static_cast<uint8_t>(0);
    created->radar = entry.radar ? static_cast<uint8_t>(1) : static_cast<uint8_t>(0);
    luaL_getmetatable(lua, "oa.brain.contact");
    if (lua_type(lua, -1) == LUA_TTABLE)
        lua_setmetatable(lua, -2);
    else
        lua_pop(lua, 1);
    lua_pushvalue(lua, -1);
    lua_rawseti(lua, cache, static_cast<lua_Integer>(entry.unit));
    lua_remove(lua, cache);
}

void remember_contacts(lua_State* lua, Seat& seat) {
    if (seat.delivered_contacts == nullptr)
        return;
    for (uint32_t row = 0; row < seat.delivered_contact_count; ++row) {
        push_contact(lua, seat, seat.delivered_contacts[row]);
        lua_pop(lua, 1);
    }
}

void install_sight(lua_State* lua, Seat& seat) {
    store_seat(lua, seat);
    if (seat.match != nullptr)
        seat.match->keep_player_contacts(seat.player, true);
    seat_may_see = seat_sees;
    install_type(lua, "oa.brain.contact", contact_index, contact_newindex);
    install_type(lua, "oa.brain.grid", grid_index, grid_newindex);
    install_type(lua, world_units_meta, units_index, units_newindex);

    add_me_getter(lua, "full_awareness", getter_full_awareness);
    add_me_getter(lua, "seen", getter_seen);
    add_me_getter(lua, "seen_tick", getter_seen_tick);
    add_me_getter(lua, "sight", getter_sight);
    add_me_getter(lua, "explored", getter_explored);
    add_me_getter(lua, "radar", getter_radar);
    add_me_getter(lua, "radar_tick", getter_radar_tick);
    add_me_method(lua, "sees", method_sees);
    add_me_method(lua, "sees_unit", method_sees_unit);

    if (!seat.full_awareness)
        return;
    add_me_method(lua, "nearest_enemy", method_nearest_enemy);
    add_me_method(lua, "seen_raw", method_seen_raw);
    auto* body = static_cast<UnitsBody*>(lua_newuserdatauv(lua, sizeof(UnitsBody), 0));
    body->unused = 0;
    luaL_getmetatable(lua, world_units_meta);
    if (lua_type(lua, -1) == LUA_TTABLE)
        lua_setmetatable(lua, -2);
    else
        lua_pop(lua, 1);
    set_game_field(lua, seat, "units");
}

} // namespace oa::sim::brains
