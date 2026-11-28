// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// `me:unit_count_of` and `game.unit_limit`. A count is the seat's own units
// only, whatever its awareness. A unit name, a record or a type index counts
// that type. A tag sums the types whose Category lines carry the word, the
// words select-by-tag, the Ctrl+letter selections and the keys editor's Tags
// use. The seat's units are tallied by type at most once for one state of
// them, and the tags are gathered from the match's unit definitions the
// first time one is asked.

#include "definitions.hpp"
#include "handles_internal.hpp"
#include "internal.hpp"
#include "lua_game.hpp"

#include "oa/core/world.h"
#include "oa/data/script_host/script_host.hpp"
#include "oa/data/unit_definitions.hpp"
#include "oa/sim/match_runtime.hpp"

#include "lua.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <new>
#include <string_view>

namespace oa::sim::brains {
namespace {

/// Registry field of the tags: each folded word to the type indexes whose
/// Category lines carry it, in type order.
constexpr const char* tag_table = "oa.brain.tags";
/// The registry keeps a category word to 255 bytes, so a longer text is no tag.
constexpr std::size_t tag_room = 256;
/// Slots of the seat's range one unit of charge walks.
constexpr uint64_t slots_per_charge = 64;

constexpr const char* usage =
    "unit_count_of expects a unit name, a tag, a unit record or a type index";

/// Lowers ASCII letters into `dest`. False for an empty text or one past the room.
bool fold_word(char* dest, std::size_t room, const char* text, std::size_t length) noexcept {
    if (dest == nullptr || text == nullptr || length == 0 || length + 1 > room)
        return false;
    for (std::size_t i = 0; i < length; ++i) {
        const auto letter = static_cast<unsigned char>(text[i]);
        dest[i] = static_cast<char>(letter >= 'A' && letter <= 'Z' ? letter - 'A' + 'a' : letter);
    }
    dest[length] = '\0';
    return true;
}

/// The seat's units by type index, counted again only when a unit was made
/// or freed since the last count, or the tick moved on.
const uint32_t* own_types(lua_State* lua, Seat& seat) {
    oa::World& world = seat.match->state();
    const oa::Player& record = world.game.players[seat.player];
    UnitTally& tally = seat.tally;
    const uint32_t types = world.unit_def_count;
    if (tally.counted && tally.types == types && tally.tick == world.game.tick &&
        tally.created == record.units_created && tally.live == record.unit_count)
        return tally.by_type.get();
    if (tally.by_type == nullptr || tally.types != types) {
        auto* memory = new (std::nothrow) uint32_t[types == 0 ? 1 : types]();
        if (memory == nullptr) {
            luaL_error(lua, "unit_count_of: not enough memory to count the units");
            return nullptr;
        }
        tally.by_type.reset(memory);
        tally.types = types;
    } else {
        std::fill_n(tally.by_type.get(), types, 0u);
    }
    uint32_t span = 0;
    const oa::Unit* first = oa::world_player_units(&world, &record, &span);
    for (uint32_t step = 0; first != nullptr && step < span; ++step) {
        const uint16_t type = first[step].type_index;
        if (type != 0 && type < types)
            ++tally.by_type[type];
    }
    tally.tick = world.game.tick;
    tally.created = record.units_created;
    tally.live = record.unit_count;
    tally.counted = true;
    script_host::charge_work(lua, (span + slots_per_charge - 1) / slots_per_charge);
    return tally.by_type.get();
}

/// Builds the tag table and leaves it on the stack. Each word of a type's
/// Category line costs one unit, and each new word its bytes.
void build_tags(lua_State* lua, const Seat& seat) {
    lua_newtable(lua);
    const int tags = lua_gettop(lua);
    uint64_t units = 0;
    const oa::World& world = seat.match->state();
    const uint32_t types = std::min<uint32_t>(world.unit_def_count, 0x10000u);
    for (uint32_t type = 1; type < types; ++type) {
        const data::unit_definitions::UnitDefinition* definition =
            seat.match->unit_definition(static_cast<uint16_t>(type));
        if (definition == nullptr)
            continue;
        for (const auto& word : definition->categories) {
            char folded[tag_room];
            if (!fold_word(folded, sizeof folded, word.data(), word.size()))
                continue;
            units += script_host::element_charge;
            lua_pushlstring(lua, folded, word.size());
            lua_rawget(lua, tags);
            if (lua_isnil(lua, -1)) {
                lua_pop(lua, 1);
                lua_newtable(lua);
                lua_pushlstring(lua, folded, word.size());
                lua_pushvalue(lua, -2);
                lua_rawset(lua, tags);
                units += script_host::units_for_bytes(word.size());
            }
            // Types are walked in order, so a word a line repeats is the last entry.
            const lua_Integer held = static_cast<lua_Integer>(lua_rawlen(lua, -1));
            bool repeated = false;
            if (held > 0) {
                lua_rawgeti(lua, -1, held);
                repeated = lua_tointeger(lua, -1) == static_cast<lua_Integer>(type);
                lua_pop(lua, 1);
            }
            if (!repeated) {
                lua_pushinteger(lua, static_cast<lua_Integer>(type));
                lua_rawseti(lua, -2, held + 1);
            }
            lua_pop(lua, 1);
        }
    }
    lua_pushvalue(lua, tags);
    lua_setfield(lua, LUA_REGISTRYINDEX, tag_table);
    script_host::charge_work(lua, units);
}

/// Pushes the types that carry `folded`, or nil when no type does.
void push_tag(lua_State* lua, const Seat& seat, const char* folded, std::size_t length) {
    lua_getfield(lua, LUA_REGISTRYINDEX, tag_table);
    if (!lua_istable(lua, -1)) {
        lua_pop(lua, 1);
        build_tags(lua, seat);
    }
    lua_pushlstring(lua, folded, length);
    lua_rawget(lua, -2);
    lua_remove(lua, -2);
}

extern "C" {

int method_unit_count_of(lua_State* lua) {
    uint16_t type = 0;
    const UnitLookup lookup = find_unit_type(lua, 2, &type);
    if (lookup == UnitLookup::not_a_type)
        return luaL_error(lua, "%s", usage);
    script_host::charge_work(lua, script_host::element_charge);
    Seat* seat = seat_of(lua);
    if (seat == nullptr || seat->match == nullptr || seat->player >= OA_PLAYER_COUNT) {
        lua_pushnil(lua);
        return 1;
    }
    if (lookup == UnitLookup::found) {
        const uint32_t* counts = own_types(lua, *seat);
        if (counts == nullptr)
            lua_pushnil(lua);
        else
            lua_pushinteger(lua, static_cast<lua_Integer>(counts[type]));
        return 1;
    }
    // A record or an index outside the table names nothing. A string that is
    // no unit's name is a tag.
    std::size_t length = 0;
    const char* text = lua_type(lua, 2) == LUA_TSTRING ? lua_tolstring(lua, 2, &length) : nullptr;
    char folded[tag_room];
    if (!fold_word(folded, sizeof folded, text, length)) {
        lua_pushnil(lua);
        return 1;
    }
    // ALL holds every type, as the category registry's own ALL does.
    if (std::string_view(folded, length) == "all") {
        const oa::Player& record = seat->match->state().game.players[seat->player];
        lua_pushinteger(lua, static_cast<lua_Integer>(record.unit_count));
        return 1;
    }
    push_tag(lua, *seat, folded, length);
    if (!lua_istable(lua, -1)) {
        lua_pop(lua, 1);
        lua_pushnil(lua);
        return 1;
    }
    const lua_Integer entries = static_cast<lua_Integer>(lua_rawlen(lua, -1));
    script_host::charge_work(lua, static_cast<uint64_t>(entries) * script_host::element_charge);
    const uint32_t* counts = own_types(lua, *seat);
    if (counts == nullptr) {
        lua_pop(lua, 1);
        lua_pushnil(lua);
        return 1;
    }
    const uint32_t types = seat->tally.types;
    lua_Integer total = 0;
    for (lua_Integer entry = 1; entry <= entries; ++entry) {
        lua_rawgeti(lua, -1, entry);
        const lua_Integer held = lua_tointeger(lua, -1);
        lua_pop(lua, 1);
        if (held > 0 && static_cast<uint64_t>(held) < types)
            total += static_cast<lua_Integer>(counts[held]);
    }
    lua_pop(lua, 1);
    lua_pushinteger(lua, total);
    return 1;
}

} // extern "C"

} // namespace

void install_unit_counts(lua_State* state, Seat& seat) {
    add_me_method(state, "unit_count_of", method_unit_count_of);
    // The match lays out each player's units once, Game.units_per_player
    // slots apiece, and a unit is made only in a free slot of its owner's
    // range. That is 3.1c's limit, or Enhanced Mode's larger one.
    if (seat.match != nullptr)
        lua_pushinteger(state, static_cast<lua_Integer>(seat.match->state().game.units_per_player));
    else
        lua_pushnil(state);
    set_game_field(state, seat, "unit_limit");
}

} // namespace oa::sim::brains
