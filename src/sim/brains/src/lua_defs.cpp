// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The brain's units, weapons and features. Names are a registry table built
// once here, so a later lookup does not walk the match's records.
#include "definitions.hpp"

#include "handles_internal.hpp"
#include "internal.hpp"

#include "oa/core/unit.h"
#include "oa/core/world.h"
#include "oa/data/field_schema.hpp"
#include "oa/data/script_host/f32.hpp"
#include "oa/data/script_host/fixed.hpp"
#include "oa/data/script_host/script_host.hpp"
#include "oa/sim/brains/handles.hpp"
#include "oa/sim/combat_state.hpp"
#include "oa/sim/match_runtime.hpp"

#include "lua.hpp"

#include <cstdio>
#include <cstring>

namespace oa::sim::brains {
namespace {

constexpr std::size_t name_room = 128;
constexpr int field_charge = 2;
constexpr int name_charge = 4;

constexpr const char* unit_meta = "oa.brain.unit_def";
constexpr const char* weapon_meta = "oa.brain.weapon_def";
constexpr const char* feature_meta = "oa.brain.feature_def";
constexpr const char* units_meta = "oa.brain.units";
constexpr const char* weapons_meta = "oa.brain.weapons";
constexpr const char* features_meta = "oa.brain.features";
constexpr const char* unit_names = "oa.brain.unit_names";
constexpr const char* weapon_names = "oa.brain.weapon_names";
constexpr const char* feature_names = "oa.brain.feature_names";

enum class Kind : uint8_t { unit = 1, weapon = 2, feature = 3 };

struct CollectionBox {
    uint8_t kind{};
};

struct RecordBox {
    uint8_t kind{};
    uint16_t index{};
};

RecordBox* record_of(lua_State* lua, int index);

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

bool same_text(const char* text, std::size_t length, const char* literal) noexcept {
    if (text == nullptr || literal == nullptr)
        return false;
    const std::size_t literal_length = std::strlen(literal);
    return length == literal_length && std::memcmp(text, literal, length) == 0;
}

const char* record_meta(uint8_t kind) noexcept {
    if (kind == static_cast<uint8_t>(Kind::weapon))
        return weapon_meta;
    if (kind == static_cast<uint8_t>(Kind::feature))
        return feature_meta;
    return unit_meta;
}

const char* name_registry(uint8_t kind) noexcept {
    if (kind == static_cast<uint8_t>(Kind::weapon))
        return weapon_names;
    if (kind == static_cast<uint8_t>(Kind::feature))
        return feature_names;
    return unit_names;
}

const oa::World* world_of(lua_State* lua) noexcept {
    Seat* seat = seat_of(lua);
    if (seat == nullptr || seat->match == nullptr)
        return nullptr;
    return &seat->match->state();
}

std::string_view chars_of(const char* text, std::size_t room) noexcept {
    std::size_t length = 0;
    if (text != nullptr) {
        while (length < room && text[length] != '\0')
            ++length;
    }
    return std::string_view(text == nullptr ? "" : text, length);
}

std::span<const oa::FeatureDef> feature_table(const oa::World& world) noexcept {
    if (world.feature_defs == nullptr || world.feature_def_count == 0)
        return {};
    return std::span<const oa::FeatureDef>(world.feature_defs, world.feature_def_count);
}

RecordBox* test_record(lua_State* lua, int index) {
    return record_of(lua, index);
}

RecordBox* record_of(lua_State* lua, int index) {
    const char* metas[] = {unit_meta, weapon_meta, feature_meta};
    for (const char* meta : metas) {
        auto* box = static_cast<RecordBox*>(luaL_testudata(lua, index, meta));
        if (box != nullptr)
            return box;
    }
    return nullptr;
}

CollectionBox* test_collection(lua_State* lua, int index) {
    auto* box = static_cast<CollectionBox*>(luaL_testudata(lua, index, units_meta));
    if (box == nullptr)
        box = static_cast<CollectionBox*>(luaL_testudata(lua, index, weapons_meta));
    if (box == nullptr)
        box = static_cast<CollectionBox*>(luaL_checkudata(lua, index, features_meta));
    return box;
}

void push_record(lua_State* lua, uint8_t kind, uint16_t index) {
    auto* box = static_cast<RecordBox*>(lua_newuserdatauv(lua, sizeof(RecordBox), 0));
    box->kind = kind;
    box->index = index;
    luaL_setmetatable(lua, record_meta(kind));
}

bool read_box(
    lua_State* lua, const RecordBox& box, std::string_view key, FieldValue& out
) noexcept {
    Seat* seat = seat_of(lua);
    const oa::World* world = world_of(lua);
    if (seat == nullptr || world == nullptr)
        return false;
    if (box.kind == static_cast<uint8_t>(Kind::unit)) {
        if (world->unit_defs == nullptr || box.index >= world->unit_def_count)
            return false;
        return read_unit_field(
            world->unit_defs[box.index], seat->match->unit_definition(box.index), key, out
        );
    }
    if (box.kind == static_cast<uint8_t>(Kind::weapon)) {
        if (box.index >= OA_WEAPON_DEF_COUNT)
            return false;
        return read_weapon_field(world->game.weapon_defs[box.index], key, out);
    }
    if (world->feature_defs == nullptr || box.index >= world->feature_def_count)
        return false;
    return read_feature_field(world->feature_defs[box.index], feature_table(*world), key, out);
}

void push_categories(lua_State* lua, const FieldValue& value) {
    lua_newtable(lua);
    if (value.listed == nullptr)
        return;
    const auto& categories = value.listed->categories;
    for (std::size_t row = 0; row < categories.size(); ++row) {
        const auto& item = categories[row];
        script_host::charge_work(
            lua, script_host::element_charge + script_host::units_for_bytes(item.size())
        );
        lua_pushlstring(lua, item.data(), item.size());
        lua_rawseti(lua, -2, static_cast<lua_Integer>(row + 1));
    }
}

void push_value(lua_State* lua, const FieldValue& value) {
    switch (value.kind) {
    case FieldValue::Kind::integer:
        lua_pushinteger(lua, static_cast<lua_Integer>(value.integer));
        break;
    case FieldValue::Kind::fixed:
        script_host::push_fixed(lua, value.integer);
        break;
    case FieldValue::Kind::real:
        script_host::push_f32(lua, value.real);
        break;
    case FieldValue::Kind::boolean:
        lua_pushboolean(lua, value.boolean ? 1 : 0);
        break;
    case FieldValue::Kind::text:
        script_host::charge_work(lua, script_host::units_for_bytes(value.text.size()));
        lua_pushlstring(
            lua, value.text.data() == nullptr ? "" : value.text.data(), value.text.size()
        );
        break;
    case FieldValue::Kind::list:
        push_categories(lua, value);
        break;
    case FieldValue::Kind::nil:
        lua_pushnil(lua);
        break;
    }
}

void push_as_fixed(lua_State* lua, const FieldValue& value) {
    if (value.kind == FieldValue::Kind::fixed) {
        script_host::push_fixed(lua, value.integer);
        return;
    }
    if (value.kind == FieldValue::Kind::integer) {
        script_host::push_fixed(lua, value.integer * static_cast<int64_t>(65536));
        return;
    }
    if (value.kind == FieldValue::Kind::real) {
        const auto count = script_host::fixed_from_number(static_cast<double>(value.real));
        if (!count) {
            lua_pushnil(lua);
            return;
        }
        script_host::push_fixed(lua, *count);
        return;
    }
    lua_pushnil(lua);
}

int method_get(lua_State* lua) {
    const RecordBox* box = test_record(lua, 1);
    if (box == nullptr || lua_type(lua, 2) != LUA_TSTRING)
        return luaL_error(lua, "get expects a field name");
    std::size_t length = 0;
    const char* text = lua_tolstring(lua, 2, &length);
    script_host::charge_work(lua, field_charge);
    FieldValue value;
    if (text == nullptr || !read_box(lua, *box, std::string_view(text, length), value)) {
        lua_pushnil(lua);
        return 1;
    }
    push_value(lua, value);
    return 1;
}

int method_fixed(lua_State* lua) {
    const RecordBox* box = test_record(lua, 1);
    if (box == nullptr || lua_type(lua, 2) != LUA_TSTRING)
        return luaL_error(lua, "fixed expects a field name");
    std::size_t length = 0;
    const char* text = lua_tolstring(lua, 2, &length);
    script_host::charge_work(lua, field_charge);
    FieldValue value;
    if (text == nullptr || !read_box(lua, *box, std::string_view(text, length), value)) {
        lua_pushnil(lua);
        return 1;
    }
    push_as_fixed(lua, value);
    return 1;
}

void push_key_row(lua_State* lua, std::string_view key, lua_Integer row) {
    script_host::charge_work(lua, script_host::element_charge);
    lua_pushlstring(lua, key.data(), key.size());
    lua_rawseti(lua, -2, row);
}

int method_keys(lua_State* lua) {
    const RecordBox* box = test_record(lua, 1);
    if (box == nullptr)
        return luaL_error(lua, "keys expects a record");
    lua_newtable(lua);
    lua_Integer row = 1;
    if (box->kind == static_cast<uint8_t>(Kind::weapon)) {
        for (const auto& spec :
             data::field_schema::fields(data::field_schema::RecordKind::weapon)) {
            if (omitted_weapon_reason(spec.key) != nullptr)
                continue;
            push_key_row(lua, spec.key, row);
            ++row;
        }
        return 1;
    }
    if (box->kind == static_cast<uint8_t>(Kind::feature)) {
        for (const std::string_view key : feature_field_keys()) {
            push_key_row(lua, key, row);
            ++row;
        }
        return 1;
    }
    for (const auto& spec : data::field_schema::fields(data::field_schema::RecordKind::unit)) {
        if (omitted_unit_reason(spec.key) != nullptr)
            continue;
        push_key_row(lua, spec.key, row);
        ++row;
    }
    return 1;
}

int method_index(lua_State* lua) {
    const RecordBox* box = test_record(lua, 1);
    if (box == nullptr)
        return luaL_error(lua, "index expects a record");
    lua_pushinteger(lua, static_cast<lua_Integer>(box->index));
    return 1;
}

int method_damage(lua_State* lua) {
    const RecordBox* box = record_of(lua, 1);
    if (box == nullptr || box->kind != static_cast<uint8_t>(Kind::weapon))
        return luaL_error(lua, "damage expects a weapon");
    const bool none = lua_gettop(lua) < 2 || lua_isnil(lua, 2);
    uint16_t type = 0;
    if (!none) {
        const bool unit_record = luaL_testudata(lua, 2, unit_meta) != nullptr;
        const bool named = lua_type(lua, 2) == LUA_TSTRING || lua_isinteger(lua, 2);
        if (!unit_record && !named)
            return luaL_error(lua, "damage expects a unit type or nothing");
        if (!resolve_unit_type(lua, 2, "damage", &type))
            return 0;
    }
    Seat* seat = seat_of(lua);
    if (seat == nullptr || seat->match == nullptr)
        return 0;
    const auto* weapon = seat->match->weapon_definition(static_cast<uint8_t>(box->index));
    if (weapon == nullptr)
        return 0;
    int32_t amount = static_cast<int32_t>(weapon->default_damage);
    if (!none) {
        const auto* definition = seat->match->unit_definition(type);
        std::string_view name{};
        const oa::World& world = seat->match->state();
        if (definition != nullptr) {
            name = std::string_view(definition->unit_name.data(), definition->unit_name.size());
        } else if (world.unit_defs != nullptr && type < world.unit_def_count) {
            name =
                chars_of(world.unit_defs[type].unit_name, sizeof world.unit_defs[type].unit_name);
        }
        amount = sim::combat_state::damage_against(*weapon, name);
    }
    lua_pushinteger(lua, static_cast<lua_Integer>(amount));
    return 1;
}

int record_index(lua_State* lua) {
    const RecordBox* box = record_of(lua, 1);
    if (box == nullptr || lua_type(lua, 2) != LUA_TSTRING)
        return 0;
    std::size_t length = 0;
    const char* text = lua_tolstring(lua, 2, &length);
    if (text == nullptr)
        return 0;
    if (same_text(text, length, "get")) {
        lua_pushcfunction(lua, method_get);
        return 1;
    }
    if (same_text(text, length, "fixed")) {
        lua_pushcfunction(lua, method_fixed);
        return 1;
    }
    if (same_text(text, length, "keys")) {
        lua_pushcfunction(lua, method_keys);
        return 1;
    }
    if (same_text(text, length, "index")) {
        lua_pushcfunction(lua, method_index);
        return 1;
    }
    if (box->kind == static_cast<uint8_t>(Kind::weapon) && same_text(text, length, "damage")) {
        lua_pushcfunction(lua, method_damage);
        return 1;
    }
    script_host::charge_work(lua, field_charge);
    FieldValue value;
    if (!read_box(lua, *box, std::string_view(text, length), value)) {
        lua_pushnil(lua);
        return 1;
    }
    push_value(lua, value);
    return 1;
}

int record_newindex(lua_State* lua) {
    return luaL_error(lua, "a record cannot be assigned");
}

bool lookup_name(
    lua_State* lua, uint8_t kind, const char* text, std::size_t length, uint16_t* index
) {
    char folded[name_room];
    if (index == nullptr || !fold_name(folded, sizeof folded, text, length) || folded[0] == '\0')
        return false;
    lua_getfield(lua, LUA_REGISTRYINDEX, name_registry(kind));
    if (!lua_istable(lua, -1)) {
        lua_pop(lua, 1);
        return false;
    }
    lua_pushlstring(lua, folded, length);
    lua_rawget(lua, -2);
    const bool found = lua_isinteger(lua, -1);
    if (found)
        *index = static_cast<uint16_t>(lua_tointeger(lua, -1));
    lua_pop(lua, 2);
    return found;
}

bool record_at(lua_State* lua, uint8_t kind, lua_Integer number, uint16_t* index) noexcept {
    const oa::World* world = world_of(lua);
    if (world == nullptr || index == nullptr)
        return false;
    if (kind == static_cast<uint8_t>(Kind::unit)) {
        if (world->unit_defs == nullptr || number < 1 ||
            static_cast<uint32_t>(number) >= world->unit_def_count)
            return false;
        *index = static_cast<uint16_t>(number);
        return true;
    }
    if (kind == static_cast<uint8_t>(Kind::feature)) {
        if (world->feature_defs == nullptr || number < 1 ||
            static_cast<uint32_t>(number) > world->feature_def_count)
            return false;
        *index = static_cast<uint16_t>(number - 1);
        return true;
    }
    if (number < 0 || number >= OA_WEAPON_DEF_COUNT)
        return false;
    if (world->game.weapon_defs[static_cast<std::size_t>(number)].key[0] == '\0')
        return false;
    *index = static_cast<uint16_t>(number);
    return true;
}

bool next_record(
    lua_State* lua, uint8_t kind, lua_Integer cursor, lua_Integer* key, uint16_t* index
) {
    const oa::World* world = world_of(lua);
    if (world == nullptr || key == nullptr || index == nullptr)
        return false;
    if (kind == static_cast<uint8_t>(Kind::weapon)) {
        const int start = static_cast<int>(cursor) + 1;
        for (int slot = start; slot < OA_WEAPON_DEF_COUNT; ++slot) {
            if (world->game.weapon_defs[slot].key[0] == '\0')
                continue;
            *key = slot;
            *index = static_cast<uint16_t>(slot);
            return true;
        }
        return false;
    }
    const lua_Integer next = cursor + 1;
    if (!record_at(lua, kind, next, index))
        return false;
    *key = next;
    return true;
}

int collection_each(lua_State* lua);

lua_Integer collection_length(lua_State* lua, uint8_t kind) noexcept {
    const oa::World* world = world_of(lua);
    if (world == nullptr)
        return 0;
    if (kind == static_cast<uint8_t>(Kind::unit)) {
        if (world->unit_defs == nullptr || world->unit_def_count == 0)
            return 0;
        return static_cast<lua_Integer>(world->unit_def_count - 1);
    }
    if (kind == static_cast<uint8_t>(Kind::feature)) {
        if (world->feature_defs == nullptr)
            return 0;
        return static_cast<lua_Integer>(world->feature_def_count);
    }
    lua_Integer used = 0;
    for (int slot = 0; slot < OA_WEAPON_DEF_COUNT; ++slot) {
        if (world->game.weapon_defs[slot].key[0] != '\0')
            ++used;
    }
    return used;
}

int collection_index(lua_State* lua) {
    const CollectionBox* box = test_collection(lua, 1);
    if (box == nullptr)
        return 0;
    if (lua_type(lua, 2) == LUA_TSTRING) {
        std::size_t length = 0;
        const char* text = lua_tolstring(lua, 2, &length);
        if (text != nullptr && same_text(text, length, "each")) {
            lua_pushcfunction(lua, collection_each);
            return 1;
        }
        script_host::charge_work(lua, script_host::units_for_bytes(length) + name_charge);
        uint16_t index = 0;
        if (text == nullptr || !lookup_name(lua, box->kind, text, length, &index))
            return 0;
        push_record(lua, box->kind, index);
        return 1;
    }
    if (!lua_isinteger(lua, 2))
        return 0;
    uint16_t index = 0;
    if (!record_at(lua, box->kind, lua_tointeger(lua, 2), &index))
        return 0;
    push_record(lua, box->kind, index);
    return 1;
}

int collection_newindex(lua_State* lua) {
    return luaL_error(lua, "a collection cannot be assigned");
}

int collection_len(lua_State* lua) {
    const CollectionBox* box = test_collection(lua, 1);
    if (box == nullptr) {
        lua_pushinteger(lua, 0);
        return 1;
    }
    lua_pushinteger(lua, collection_length(lua, box->kind));
    return 1;
}

int collection_each(lua_State* lua) {
    const CollectionBox* box = test_collection(lua, 1);
    if (box == nullptr || lua_type(lua, 2) != LUA_TFUNCTION)
        return luaL_error(lua, "each expects a function");
    const uint8_t kind = box->kind;
    lua_Integer cursor =
        kind == static_cast<uint8_t>(Kind::weapon) ? lua_Integer{-1} : lua_Integer{0};
    for (;;) {
        lua_Integer key = 0;
        uint16_t index = 0;
        if (!next_record(lua, kind, cursor, &key, &index))
            break;
        cursor = key;
        script_host::charge_work(lua, script_host::element_charge);
        lua_pushvalue(lua, 2);
        push_record(lua, kind, index);
        lua_call(lua, 1, 0);
    }
    return 0;
}

int pairs_next(lua_State* lua) {
    const auto kind = static_cast<uint8_t>(lua_tointeger(lua, lua_upvalueindex(1)));
    const lua_Integer cursor = lua_tointeger(lua, lua_upvalueindex(2));
    lua_Integer key = 0;
    uint16_t index = 0;
    if (!next_record(lua, kind, cursor, &key, &index))
        return 0;
    lua_pushinteger(lua, key);
    lua_replace(lua, lua_upvalueindex(2));
    script_host::charge_work(lua, script_host::element_charge);
    lua_pushinteger(lua, key);
    push_record(lua, kind, index);
    return 2;
}

int collection_pairs(lua_State* lua) {
    const CollectionBox* box = test_collection(lua, 1);
    if (box == nullptr)
        return luaL_error(lua, "pairs expects a collection");
    lua_pushinteger(lua, box->kind);
    const lua_Integer start =
        box->kind == static_cast<uint8_t>(Kind::weapon) ? lua_Integer{-1} : lua_Integer{0};
    lua_pushinteger(lua, start);
    lua_pushcclosure(lua, pairs_next, 2);
    lua_pushvalue(lua, 1);
    lua_pushnil(lua);
    return 3;
}

void set_meta(
    lua_State* lua,
    const char* name,
    lua_CFunction index,
    lua_CFunction newindex,
    lua_CFunction length,
    lua_CFunction pairs
) {
    if (luaL_newmetatable(lua, name) == 0) {
        lua_pop(lua, 1);
        return;
    }
    lua_pushcfunction(lua, index);
    lua_setfield(lua, -2, "__index");
    lua_pushcfunction(lua, newindex);
    lua_setfield(lua, -2, "__newindex");
    if (length != nullptr) {
        lua_pushcfunction(lua, length);
        lua_setfield(lua, -2, "__len");
    }
    if (pairs != nullptr) {
        lua_pushcfunction(lua, pairs);
        lua_setfield(lua, -2, "__pairs");
    }
    lua_pushboolean(lua, 0);
    lua_setfield(lua, -2, "__metatable");
    lua_pop(lua, 1);
}

void remember_name(lua_State* lua, int table, std::string_view name, lua_Integer index) {
    char folded[name_room];
    if (!fold_name(folded, sizeof folded, name.data(), name.size()) || folded[0] == '\0')
        return;
    const int absolute = lua_absindex(lua, table);
    lua_pushlstring(lua, folded, name.size());
    lua_rawget(lua, absolute);
    if (!lua_isnil(lua, -1)) {
        lua_pop(lua, 1);
        return;
    }
    lua_pop(lua, 1);
    script_host::charge_work(lua, script_host::units_for_bytes(name.size()) + name_charge);
    lua_pushlstring(lua, folded, name.size());
    lua_pushinteger(lua, index);
    lua_rawset(lua, absolute);
}

void build_names(lua_State* lua, Seat& seat) {
    lua_newtable(lua);
    lua_newtable(lua);
    lua_newtable(lua);
    const oa::World* world = seat.match == nullptr ? nullptr : &seat.match->state();
    if (world != nullptr && world->unit_defs != nullptr) {
        for (uint32_t type = 1; type < world->unit_def_count; ++type) {
            remember_name(
                lua,
                -3,
                chars_of(world->unit_defs[type].unit_name, sizeof world->unit_defs[type].unit_name),
                static_cast<lua_Integer>(type)
            );
        }
    }
    if (world != nullptr) {
        for (int slot = 0; slot < OA_WEAPON_DEF_COUNT; ++slot) {
            const oa::WeaponDef& weapon = world->game.weapon_defs[slot];
            if (weapon.key[0] == '\0')
                continue;
            remember_name(lua, -2, chars_of(weapon.key, sizeof weapon.key), slot);
        }
    }
    if (world != nullptr && world->feature_defs != nullptr) {
        for (uint32_t row = 0; row < world->feature_def_count; ++row) {
            remember_name(
                lua,
                -1,
                chars_of(world->feature_defs[row].name, sizeof world->feature_defs[row].name),
                static_cast<lua_Integer>(row)
            );
        }
    }
    lua_setfield(lua, LUA_REGISTRYINDEX, feature_names);
    lua_setfield(lua, LUA_REGISTRYINDEX, weapon_names);
    lua_setfield(lua, LUA_REGISTRYINDEX, unit_names);
}

void push_collection(lua_State* lua, uint8_t kind, const char* meta, const char* global_name) {
    auto* box = static_cast<CollectionBox*>(lua_newuserdatauv(lua, sizeof(CollectionBox), 0));
    box->kind = kind;
    luaL_setmetatable(lua, meta);
    lua_setglobal(lua, global_name);
}

bool unit_record(lua_State* lua, int index, uint16_t* type) noexcept {
    auto* box = static_cast<RecordBox*>(luaL_testudata(lua, index, unit_meta));
    if (box == nullptr || type == nullptr)
        return false;
    *type = box->index;
    return true;
}

bool type_in_table(lua_State* lua, uint16_t type) noexcept {
    const oa::World* world = world_of(lua);
    return world != nullptr && world->unit_defs != nullptr && type >= 1 &&
           type < world->unit_def_count;
}

int getter_type(lua_State* lua) {
    UnitRef ref{};
    if (check_unit(lua, 1, "type", &ref) != 0)
        return 0;
    Seat* seat = seat_of(lua);
    if (seat == nullptr || seat->match == nullptr || !unit_current(*seat->match, ref))
        return 0;
    const oa::Unit* unit = oa::world_unit_at(&seat->match->state(), ref.slot);
    if (unit == nullptr || unit->type_index == 0)
        return 0;
    push_record(lua, static_cast<uint8_t>(Kind::unit), unit->type_index);
    return 1;
}

} // namespace

bool resolve_unit_type(lua_State* lua, int index, const char* function, uint16_t* type) {
    const char* name = function == nullptr ? "type" : function;
    uint16_t found = 0;
    if (unit_record(lua, index, &found)) {
        if (type_in_table(lua, found)) {
            if (type != nullptr)
                *type = found;
            return true;
        }
        char text[32];
        std::snprintf(text, sizeof text, "%u", static_cast<unsigned>(found));
        luaL_error(lua, "%s: unknown unit type %s", name, text);
    }
    if (lua_type(lua, index) == LUA_TSTRING) {
        std::size_t length = 0;
        const char* text = lua_tolstring(lua, index, &length);
        if (text != nullptr &&
            lookup_name(lua, static_cast<uint8_t>(Kind::unit), text, length, &found) &&
            type_in_table(lua, found)) {
            if (type != nullptr)
                *type = found;
            return true;
        }
        luaL_error(lua, "%s: unknown unit type %s", name, text == nullptr ? "" : text);
    }
    if (lua_isinteger(lua, index)) {
        const lua_Integer number = lua_tointeger(lua, index);
        if (number >= 1 && number <= 0xffff && type_in_table(lua, static_cast<uint16_t>(number))) {
            if (type != nullptr)
                *type = static_cast<uint16_t>(number);
            return true;
        }
        char text[32];
        std::snprintf(text, sizeof text, "%lld", static_cast<long long>(number));
        luaL_error(lua, "%s: unknown unit type %s", name, text);
    }
    return luaL_error(lua, "%s expects a unit type: a record, a name or an index", name) != 0;
}

void install_definitions(lua_State* lua, Seat& seat) {
    store_seat(lua, seat);
    set_meta(lua, unit_meta, record_index, record_newindex, nullptr, nullptr);
    set_meta(lua, weapon_meta, record_index, record_newindex, nullptr, nullptr);
    set_meta(lua, feature_meta, record_index, record_newindex, nullptr, nullptr);
    set_meta(
        lua, units_meta, collection_index, collection_newindex, collection_len, collection_pairs
    );
    set_meta(
        lua, weapons_meta, collection_index, collection_newindex, collection_len, collection_pairs
    );
    set_meta(
        lua, features_meta, collection_index, collection_newindex, collection_len, collection_pairs
    );
    build_names(lua, seat);
    push_collection(lua, static_cast<uint8_t>(Kind::unit), units_meta, "units");
    push_collection(lua, static_cast<uint8_t>(Kind::weapon), weapons_meta, "weapons");
    push_collection(lua, static_cast<uint8_t>(Kind::feature), features_meta, "features");
    add_unit_getter(lua, "type", Access::seen, getter_type);
}

} // namespace oa::sim::brains
