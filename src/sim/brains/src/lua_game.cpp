// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Builds `game` from the seat's facts. The walk reads the profile copy on
// those facts, or a default profile when the copy is absent, and keeps every
// simulation hack whether it is on or off. View hacks are left out. The
// charge is counted here and spent once the builder's locals are gone: this
// runs in the sandbox's open phase, which has its own allowance.

#include "lua_game.hpp"

#include "read_only.hpp"

#include "oa/data/mod_profile/bindings.hpp"
#include "oa/data/mod_profile/registry.hpp"
#include "oa/data/script_host/script_host.hpp"

#include "oa/core/game_state.h"

#include "lua.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>

namespace oa::sim::brains {
namespace {

constexpr const char* denial = "game is read-only";

namespace registry = oa::data::mod_profile::registry;
namespace binding_types = oa::data::mod_profile::binding_types;

void push_enum_name(
    lua_State* state, const registry::ValueSpec* spec, std::size_t index, uint64_t* units
) {
    if (spec == nullptr) {
        lua_pushlstring(state, "", 0);
        *units += script_host::element_charge;
        return;
    }
    const auto names = registry::enum_values_of(*spec);
    if (index >= names.size()) {
        lua_pushlstring(state, "", 0);
        *units += script_host::element_charge;
        return;
    }
    const auto name = names[index];
    lua_pushlstring(state, name.data(), name.size());
    *units += script_host::element_charge + script_host::units_for_bytes(name.size());
}

template <class Field>
void push_field(
    lua_State* state,
    const Field& field,
    const registry::ValueSpec* spec,
    uint64_t* units,
    const char* text
);

template <class Field>
void push_field(
    lua_State* state,
    const Field& field,
    const registry::ValueSpec* spec,
    uint64_t* units,
    const char* text
) {
    luaL_checkstack(state, 8, "game");
    if constexpr (std::is_same_v<Field, bool>) {
        lua_pushboolean(state, field ? 1 : 0);
        *units += script_host::element_charge;
    } else if constexpr (std::is_same_v<Field, double>) {
        lua_pushnumber(state, field);
        *units += script_host::element_charge;
    } else if constexpr (std::is_enum_v<Field>) {
        push_enum_name(
            state,
            spec,
            static_cast<std::size_t>(static_cast<std::underlying_type_t<Field>>(field)),
            units
        );
    } else if constexpr (std::is_integral_v<Field>) {
        lua_pushinteger(state, static_cast<lua_Integer>(field));
        *units += script_host::element_charge;
    } else if constexpr (binding_types::is_fixed_text<Field>::value) {
        const auto held = field.view();
        lua_pushlstring(state, held.data(), held.size());
        *units += script_host::element_charge + script_host::units_for_bytes(held.size());
    } else if constexpr (requires(const Field& item) {
                             item.c_str();
                             item.size();
                         }) {
        lua_pushlstring(state, field.c_str(), field.size());
        *units += script_host::element_charge + script_host::units_for_bytes(field.size());
    } else if constexpr (binding_types::is_optional<Field>::value) {
        if (!field.has_value()) {
            lua_pushlstring(state, "none", 4);
            *units += script_host::element_charge + script_host::units_for_bytes(4);
        } else {
            push_field(state, *field, spec, units, text);
        }
    } else if constexpr (binding_types::is_enum_set<Field>::value) {
        lua_newtable(state);
        *units += script_host::element_charge;
        const auto names =
            spec != nullptr ? registry::enum_values_of(*spec) : std::span<const std::string_view>{};
        lua_Integer slot = 1;
        for (std::size_t member = 0; member < field.members.size(); ++member) {
            if (!field.members[member])
                continue;
            if (member < names.size()) {
                lua_pushlstring(state, names[member].data(), names[member].size());
                *units += script_host::element_charge +
                          script_host::units_for_bytes(names[member].size());
            } else {
                lua_pushlstring(state, "", 0);
                *units += script_host::element_charge;
            }
            lua_rawseti(state, -2, slot);
            ++slot;
        }
        push_read_only(state, text);
    } else if constexpr (
        binding_types::is_array<Field>::value || binding_types::is_vector<Field>::value ||
        binding_types::is_fixed_list<Field>::value
    ) {
        lua_newtable(state);
        *units += script_host::element_charge;
        if constexpr (binding_types::is_fixed_list<Field>::value) {
            for (std::size_t item = 0; item < field.count; ++item) {
                push_field(state, field.items[item], spec, units, text);
                lua_rawseti(state, -2, static_cast<lua_Integer>(item + 1));
            }
        } else {
            for (std::size_t item = 0; item < field.size(); ++item) {
                push_field(state, field[item], spec, units, text);
                lua_rawseti(state, -2, static_cast<lua_Integer>(item + 1));
            }
        }
        push_read_only(state, text);
    } else {
        static_assert(
            sizeof(Field) == 0, "a record field of a type the game table does not handle"
        );
    }
}

struct HackWalk {
    lua_State* state{};
    int hacks{};
    int limits{};
    int current{};
    const char* current_id{};
    bool current_hack{};
    uint64_t* units{};
    const char* text{denial};

    void seal() {
        if (current == 0 || current_id == nullptr)
            return;
        luaL_checkstack(state, 4, "game");
        lua_pushvalue(state, current);
        push_read_only(state, text);
        const int parent = current_hack ? hacks : limits;
        lua_setfield(state, parent, current_id);
        *units += script_host::units_for_bytes(std::strlen(current_id));
        current = 0;
        current_id = nullptr;
    }

    void begin(const char* id, bool hack_table) {
        if (current != 0 && current_id != nullptr && std::strcmp(current_id, id) == 0 &&
            current_hack == hack_table)
            return;
        seal();
        luaL_checkstack(state, 4, "game");
        lua_newtable(state);
        current = lua_gettop(state);
        current_id = id;
        current_hack = hack_table;
        *units += script_host::element_charge;
    }

    void hack(const char* id, bool enabled) {
        const registry::Entry* entry = registry::find_entry(id);
        if (entry == nullptr || entry->kind != registry::EntryKind::hack ||
            entry->scope != registry::Scope::sim)
            return;
        begin(id, true);
        lua_pushboolean(state, enabled ? 1 : 0);
        lua_setfield(state, current, "on");
        *units += script_host::element_charge + script_host::units_for_bytes(2);
    }

    template <class Field>
    void parameter(const char* id, const char* name, const Field& field) {
        const registry::Entry* entry = registry::find_entry(id);
        if (entry == nullptr)
            return;
        const bool limit = entry->kind == registry::EntryKind::limit;
        const bool sim_hack =
            entry->kind == registry::EntryKind::hack && entry->scope == registry::Scope::sim;
        if (!limit && !sim_hack)
            return;
        begin(id, sim_hack);
        const registry::ValueSpec* spec = nullptr;
        const int found = registry::find_parameter(*entry, name);
        if (found >= 0) {
            const auto parameters = registry::parameters_of(*entry);
            if (static_cast<std::size_t>(found) < parameters.size())
                spec = &parameters[static_cast<std::size_t>(found)].value;
        }
        push_field(state, field, spec, units, text);
        lua_setfield(state, current, name);
        *units += script_host::units_for_bytes(std::strlen(name));
    }

    template <class Field>
    void value(const char*, const Field&) {}

    template <class Field>
    void data_key(const char*, const Field&) {}
};

void set_text(
    lua_State* state,
    int table,
    const char* key,
    const char* value,
    std::size_t size,
    uint64_t* units
) {
    luaL_checkstack(state, 4, "game");
    lua_pushlstring(state, value != nullptr ? value : "", value != nullptr ? size : 0);
    lua_setfield(state, table, key);
    *units +=
        script_host::element_charge + script_host::units_for_bytes(value != nullptr ? size : 0);
    *units += script_host::units_for_bytes(std::strlen(key));
}

template <class Text>
void set_owned_text(
    lua_State* state, int table, const char* key, const Text& value, uint64_t* units
) {
    set_text(state, table, key, value.data(), value.size(), units);
}

void set_bool(lua_State* state, int table, const char* key, bool value, uint64_t* units) {
    lua_pushboolean(state, value ? 1 : 0);
    lua_setfield(state, table, key);
    *units += script_host::element_charge + script_host::units_for_bytes(std::strlen(key));
}

void set_number(lua_State* state, int table, const char* key, double value, uint64_t* units) {
    lua_pushnumber(state, value);
    lua_setfield(state, table, key);
    *units += script_host::element_charge + script_host::units_for_bytes(std::strlen(key));
}

const char* mode_text(oa::data::brain_facts::GameMode mode) noexcept {
    if (mode == oa::data::brain_facts::GameMode::campaign)
        return "campaign";
    if (mode == oa::data::brain_facts::GameMode::network)
        return "network";
    return "skirmish";
}

const char* difficulty_name(int32_t difficulty) noexcept {
    if (difficulty == OA_DIFFICULTY_EASY)
        return "easy";
    if (difficulty == OA_DIFFICULTY_HARD)
        return "hard";
    return "medium";
}

extern "C" {

static int game_has_expansion(lua_State* state) {
    std::size_t id_size = 0;
    const char* id = nullptr;
    if (lua_type(state, 2) == LUA_TSTRING)
        id = lua_tolstring(state, 2, &id_size);
    lua_getfield(state, 1, "expansions");
    if (id == nullptr || lua_type(state, -1) != LUA_TTABLE) {
        lua_pushboolean(state, 0);
        return 1;
    }
    const int list = lua_absindex(state, -1);
    const lua_Integer count = luaL_len(state, list);
    int found = 0;
    for (lua_Integer slot = 1; slot <= count; ++slot) {
        lua_geti(state, list, slot);
        std::size_t size = 0;
        const char* text = nullptr;
        if (lua_type(state, -1) == LUA_TSTRING)
            text = lua_tolstring(state, -1, &size);
        if (text != nullptr && size == id_size && std::memcmp(text, id, size) == 0)
            found = 1;
        lua_pop(state, 1);
    }
    lua_pushboolean(state, found);
    return 1;
}

static int game_hack(lua_State* state) {
    if (lua_type(state, 2) != LUA_TSTRING) {
        lua_pushnil(state);
        return 1;
    }
    lua_getfield(state, 1, "mod");
    if (lua_type(state, -1) != LUA_TTABLE) {
        lua_pushnil(state);
        return 1;
    }
    lua_getfield(state, -1, "hacks");
    if (lua_type(state, -1) != LUA_TTABLE) {
        lua_pushnil(state);
        return 1;
    }
    lua_pushvalue(state, 2);
    lua_gettable(state, -2);
    return 1;
}

} // extern "C"

void push_sides(
    lua_State* state, int mod_table, const oa::data::brain_facts::GameFacts& facts, uint64_t* units
) {
    lua_newtable(state);
    *units += script_host::element_charge;
    const std::size_t count = facts.mod.sides.size();
    for (std::size_t side_index = 0; side_index < count; ++side_index) {
        const auto& side = facts.mod.sides[side_index];
        lua_newtable(state);
        *units += script_host::element_charge;
        const int side_table = lua_gettop(state);
        set_owned_text(state, side_table, "name", side.name, units);
        set_owned_text(state, side_table, "prefix", side.prefix, units);
        set_owned_text(state, side_table, "commander", side.commander, units);
        push_read_only(state, denial);
        lua_rawseti(state, -2, static_cast<lua_Integer>(side_index + 1));
    }
    push_read_only(state, denial);
    lua_setfield(state, mod_table, "sides");
    *units += script_host::units_for_bytes(std::strlen("sides"));
}

void push_option_value(
    lua_State* state, int options, const oa::data::brain_facts::OptionFacts& option, uint64_t* units
) {
    if (option.kind == oa::data::brain_facts::OptionFacts::Kind::toggle)
        set_bool(state, options, option.id.c_str(), option.toggle, units);
    else if (option.kind == oa::data::brain_facts::OptionFacts::Kind::number)
        set_number(state, options, option.id.c_str(), option.number, units);
    else
        set_text(state, options, option.id.c_str(), option.text.data(), option.text.size(), units);
}

void push_mutators(
    lua_State* state, int hidden, const oa::data::brain_facts::GameFacts& facts, uint64_t* units
) {
    lua_newtable(state);
    *units += script_host::element_charge;
    const std::size_t count = facts.mutators.size();
    for (std::size_t mutator_index = 0; mutator_index < count; ++mutator_index) {
        const auto& mutator = facts.mutators[mutator_index];
        lua_newtable(state);
        *units += script_host::element_charge;
        const int mutator_table = lua_gettop(state);
        set_owned_text(state, mutator_table, "id", mutator.id, units);
        set_owned_text(state, mutator_table, "version", mutator.version, units);
        lua_newtable(state);
        *units += script_host::element_charge;
        const int options = lua_gettop(state);
        const std::size_t option_count = mutator.options.size();
        for (std::size_t option_index = 0; option_index < option_count; ++option_index)
            push_option_value(state, options, mutator.options[option_index], units);
        push_read_only(state, denial);
        lua_setfield(state, mutator_table, "options");
        *units += script_host::units_for_bytes(std::strlen("options"));
        push_read_only(state, denial);
        lua_rawseti(state, -2, static_cast<lua_Integer>(mutator_index + 1));
    }
    push_read_only(state, denial);
    lua_setfield(state, hidden, "mutators");
    *units += script_host::units_for_bytes(std::strlen("mutators"));
}

void push_mod(
    lua_State* state,
    int hidden,
    const oa::data::brain_facts::GameFacts& facts,
    const oa::data::mod_profile::ModProfile& played,
    uint64_t* units
) {
    lua_newtable(state);
    *units += script_host::element_charge;
    const int mod_table = lua_gettop(state);
    set_owned_text(state, mod_table, "id", facts.mod.id, units);
    set_owned_text(state, mod_table, "name", facts.mod.name, units);
    set_owned_text(state, mod_table, "version", facts.mod.version, units);
    push_sides(state, mod_table, facts, units);

    lua_newtable(state);
    const int hacks = lua_gettop(state);
    *units += script_host::element_charge;
    lua_newtable(state);
    const int limits = lua_gettop(state);
    *units += script_host::element_charge;

    HackWalk walk;
    walk.state = state;
    walk.hacks = hacks;
    walk.limits = limits;
    walk.units = units;
    oa::data::mod_profile::visit_bindings(walk, played);
    walk.seal();

    lua_pushvalue(state, hacks);
    push_read_only(state, denial);
    lua_setfield(state, mod_table, "hacks");
    *units += script_host::units_for_bytes(std::strlen("hacks"));
    lua_pushvalue(state, limits);
    push_read_only(state, denial);
    lua_setfield(state, mod_table, "limits");
    *units += script_host::units_for_bytes(std::strlen("limits"));
    lua_settop(state, mod_table);

    push_read_only(state, denial);
    lua_setfield(state, hidden, "mod");
    *units += script_host::units_for_bytes(std::strlen("mod"));
}

void fill_hidden(
    lua_State* state,
    int hidden,
    const Seat& seat,
    const oa::data::brain_facts::GameFacts& facts,
    const oa::data::mod_profile::ModProfile& played,
    uint64_t* units
) {
    set_owned_text(state, hidden, "base", facts.base, units);
    lua_newtable(state);
    *units += script_host::element_charge;
    const std::size_t expansion_count = facts.expansions.size();
    for (std::size_t expansion_index = 0; expansion_index < expansion_count; ++expansion_index) {
        const auto& expansion = facts.expansions[expansion_index];
        lua_pushlstring(state, expansion.data(), expansion.size());
        lua_rawseti(state, -2, static_cast<lua_Integer>(expansion_index + 1));
        *units += script_host::element_charge + script_host::units_for_bytes(expansion.size());
    }
    push_read_only(state, denial);
    lua_setfield(state, hidden, "expansions");
    *units += script_host::units_for_bytes(std::strlen("expansions"));

    set_owned_text(state, hidden, "engine", facts.engine, units);
    set_text(
        state, hidden, "mode", mode_text(facts.mode), std::strlen(mode_text(facts.mode)), units
    );
    const char* difficulty = difficulty_name(facts.difficulty);
    set_text(state, hidden, "difficulty", difficulty, std::strlen(difficulty), units);
    set_bool(state, hidden, "in_match", seat.match != nullptr, units);
    push_mod(state, hidden, facts, played, units);
    push_mutators(state, hidden, facts, units);

    lua_pushcfunction(state, game_has_expansion);
    lua_setfield(state, hidden, "has_expansion");
    *units +=
        script_host::element_charge + script_host::units_for_bytes(std::strlen("has_expansion"));
    lua_pushcfunction(state, game_hack);
    lua_setfield(state, hidden, "hack");
    *units += script_host::element_charge + script_host::units_for_bytes(std::strlen("hack"));
    lua_settop(state, hidden);
}

} // namespace

void install_game(lua_State* state, Seat& seat) {
    static const oa::data::brain_facts::GameFacts fallback{};
    static const oa::data::mod_profile::ModProfile baseline{};
    const oa::data::brain_facts::GameFacts* facts = seat.facts != nullptr ? seat.facts : &fallback;
    const oa::data::mod_profile::ModProfile* played = &baseline;
    if (facts->mod.profile)
        played = &*facts->mod.profile;

    uint64_t units = 0;
    lua_getglobal(state, "game");
    if (lua_type(state, -1) != LUA_TTABLE) {
        lua_pop(state, 1);
        lua_newtable(state);
        lua_pushvalue(state, -1);
        lua_setglobal(state, "game");
    }
    const int proxy = lua_absindex(state, -1);
    lua_newtable(state);
    const int hidden = lua_gettop(state);
    fill_hidden(state, hidden, seat, *facts, *played, &units);
    seat.game_hidden_ref = luaL_ref(state, LUA_REGISTRYINDEX);
    lua_rawgeti(state, LUA_REGISTRYINDEX, seat.game_hidden_ref);
    make_read_only(state, proxy, lua_gettop(state), denial);
    lua_pop(state, 2);
    script_host::charge_work(state, units);
}

void set_game_field(lua_State* state, Seat& seat, const char* key) {
    if (seat.game_hidden_ref < 0 || key == nullptr) {
        lua_pop(state, 1);
        return;
    }
    lua_rawgeti(state, LUA_REGISTRYINDEX, seat.game_hidden_ref);
    lua_insert(state, -2);
    lua_setfield(state, -2, key);
    lua_pop(state, 1);
}

} // namespace oa::sim::brains
