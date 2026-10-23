// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The made-up brain definition-reader: unit, weapon and feature records,
// and the installed units compared with the values the match loaded.
#include "match_fixture.hpp"

#include "definitions.hpp"
#include "internal.hpp"

#include "oa/base/game_math.hpp"
#include "oa/core/feature_def.h"
#include "oa/core/player.h"
#include "oa/core/unit.h"
#include "oa/data/defs/asset_files.hpp"
#include "oa/data/defs/move_classes.hpp"
#include "oa/data/defs/unit_catalog.hpp"
#include "oa/data/defs/unit_def_loader.hpp"
#include "oa/data/defs/unit_header.hpp"
#include "oa/data/defs/unit_records.hpp"
#include "oa/data/defs/weapons.hpp"
#include "oa/data/field_schema.hpp"
#include "oa/data/mission_types.hpp"
#include "oa/sim/brains/check.hpp"
#include "oa/sim/brains/handles.hpp"
#include "oa/sim/brains/seat.hpp"
#include "oa/test/game_assets.hpp"

#include "lua.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace brains = oa::sim::brains;
namespace host = oa::data::script_host;
namespace schema = oa::data::field_schema;
namespace defs = oa::data::defs;
namespace mission = oa::data::mission_types;

int failures = 0;

void fail(const std::string& what) {
    std::cerr << "FAIL: " << what << '\n';
    ++failures;
}

void check(bool ok, const std::string& what) {
    if (!ok)
        fail(what);
}

int probe_resolve(lua_State* lua) {
    uint16_t type = 0;
    if (!brains::resolve_unit_type(lua, 1, "resolve", &type))
        return 0;
    lua_pushinteger(lua, static_cast<lua_Integer>(type));
    return 1;
}

void install_probe(void*, lua_State* lua) {
    lua_pushcfunction(lua, probe_resolve);
    lua_setglobal(lua, "resolve_type");
}

struct Published {
    lua_Integer velocity_raw{};
    lua_Integer energy_bits{};
    lua_Integer energy_shot_bits{};
    lua_Integer metal_bits{};
    lua_Integer nuke_reload{};
    lua_Integer damage_default{};
    lua_Integer damage_solar{};
};

void publish_work(void* context, lua_State* lua) {
    const auto* published = static_cast<const Published*>(context);
    if (published == nullptr)
        return;
    lua_pushinteger(lua, published->velocity_raw);
    lua_setglobal(lua, "velocity_raw");
    lua_pushinteger(lua, published->energy_bits);
    lua_setglobal(lua, "energy_bits");
    lua_pushinteger(lua, published->energy_shot_bits);
    lua_setglobal(lua, "energy_shot_bits");
    lua_pushinteger(lua, published->metal_bits);
    lua_setglobal(lua, "metal_bits");
    lua_pushinteger(lua, published->nuke_reload);
    lua_setglobal(lua, "nuke_reload");
    lua_pushinteger(lua, published->damage_default);
    lua_setglobal(lua, "damage_default");
    lua_pushinteger(lua, published->damage_solar);
    lua_setglobal(lua, "damage_solar");
}

struct PushRequest {
    brains::UnitRef ref{};
    brains::Seat* seat{};
    const char* global_name{};
};

void push_named(void* context, lua_State* lua) {
    auto* request = static_cast<PushRequest*>(context);
    if (request == nullptr || request->seat == nullptr || request->global_name == nullptr)
        return;
    brains::push_unit(lua, *request->seat, request->ref);
    lua_setglobal(lua, request->global_name);
}

int push_read(void* context, lua_State* lua) {
    auto* seat = static_cast<brains::Seat*>(context);
    if (seat == nullptr)
        return luaL_error(lua, "missing seat");
    lua_rawgeti(lua, LUA_REGISTRYINDEX, seat->hooks_ref);
    lua_getfield(lua, -1, "read_definitions");
    lua_remove(lua, -2);
    return 1;
}

std::string load_text(const char* name) {
    const std::string path = std::string(OA_DEFINITION_READER_DIR) + "/" + name;
    std::ifstream input(path);
    if (!input)
        throw std::runtime_error("missing " + path);
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

void seat_reader(
    Fixture& fixture, const std::string& script, const std::string& manifest, uint8_t which
) {
    const auto script_bytes =
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(script.data()), script.size()};
    const auto manifest_bytes = std::span<const uint8_t>{
        reinterpret_cast<const uint8_t*>(manifest.data()), manifest.size()
    };
    brains::PackageFile files[] = {
        {"brain.lua", script_bytes},
        {"oabrain.yaml", manifest_bytes},
    };
    brains::SeatSpec spec;
    spec.player = which;
    spec.id = "definition-reader";
    spec.name = "Definition Reader";
    spec.version = "1";
    spec.entry = "brain.lua";
    spec.level = "steady";
    spec.difficulty = OA_DIFFICULTY_MEDIUM;
    spec.files = files;
    const brains::SeatOutcome outcome = brains::seat_brain(*fixture.match, spec);
    if (outcome.result != brains::SeatResult::seated)
        throw std::runtime_error(std::string("seat failed: ") + outcome.reason.data());
}

lua_Integer bits_of(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    return static_cast<lua_Integer>(bits);
}

void require_schema() {
    oa::UnitDef unit{};
    oa::data::unit_definitions::UnitDefinition definition{};
    for (const auto& spec : schema::fields(schema::RecordKind::unit)) {
        brains::FieldValue value;
        const bool read = brains::read_unit_field(unit, &definition, spec.key, value);
        const char* reason = brains::omitted_unit_reason(spec.key);
        if (reason != nullptr)
            CHECK(!read);
        else
            CHECK(read);
    }
    brains::FieldValue named;
    CHECK(brains::read_unit_field(unit, &definition, "UnitName", named));
    CHECK(named.kind == brains::FieldValue::Kind::text);
    CHECK(!brains::read_unit_field(unit, &definition, "not-a-field", named));
    CHECK(brains::omitted_unit_reason("not-a-field") == nullptr);
    CHECK(brains::omitted_unit_reason("Copyright") != nullptr);

    oa::WeaponDef weapon{};
    for (const auto& spec : schema::fields(schema::RecordKind::weapon)) {
        brains::FieldValue value;
        const bool read = brains::read_weapon_field(weapon, spec.key, value);
        const char* reason = brains::omitted_weapon_reason(spec.key);
        if (reason != nullptr)
            CHECK(!read);
        else
            CHECK(read);
    }
    CHECK(brains::omitted_weapon_reason("soundstart") != nullptr);

    oa::FeatureDef feature{};
    const std::array<oa::FeatureDef, 1> table{};
    for (const std::string_view key : brains::feature_field_keys()) {
        brains::FieldValue value;
        CHECK(brains::read_feature_field(feature, table, key, value));
    }
}

void check_outside(const std::string& script, const std::string& manifest) {
    const auto script_bytes =
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(script.data()), script.size()};
    const auto manifest_bytes = std::span<const uint8_t>{
        reinterpret_cast<const uint8_t*>(manifest.data()), manifest.size()
    };
    brains::PackageFile files[] = {
        {"brain.lua", script_bytes},
        {"oabrain.yaml", manifest_bytes},
    };
    brains::SeatSpec spec;
    spec.player = computer;
    spec.id = "definition-reader";
    spec.name = "Definition Reader";
    spec.version = "1";
    spec.entry = "brain.lua";
    spec.level = "steady";
    spec.difficulty = OA_DIFFICULTY_MEDIUM;
    spec.files = files;
    oa::data::brain_facts::GameFacts facts{};
    const brains::SeatOutcome outcome = brains::check_brain(spec, facts);
    if (outcome.result != brains::SeatResult::seated)
        throw std::runtime_error(std::string("outside fit failed: ") + outcome.reason.data());
}

void read_seat(
    Fixture& fixture,
    brains::Seat& seat,
    brains::UnitRef own_ref,
    brains::UnitRef foe_ref,
    const Published& published
) {
    host::Sandbox* sandbox = brains::seat_sandbox(*fixture.match, seat.player);
    CHECK(sandbox != nullptr);
    host::ScriptError error;
    if (!sandbox->run_host("publish", const_cast<Published*>(&published), publish_work, error))
        throw std::runtime_error(error.message.empty() ? "publish failed" : error.message);
    if (!sandbox->run_host("resolve", nullptr, install_probe, error))
        throw std::runtime_error(error.message.empty() ? "resolve failed" : error.message);
    PushRequest own{own_ref, &seat, "own_unit"};
    PushRequest foe{foe_ref, &seat, "foe_unit"};
    if (!sandbox->run_host("own", &own, push_named, error))
        throw std::runtime_error(error.message.empty() ? "own unit failed" : error.message);
    if (!sandbox->run_host("foe", &foe, push_named, error))
        throw std::runtime_error(error.message.empty() ? "foe unit failed" : error.message);
    host::CallHooks hooks;
    hooks.context = &seat;
    hooks.push = push_read;
    if (!sandbox->call("read", hooks, error))
        throw std::runtime_error(error.message.empty() ? "read failed" : error.message);
}

void check_seated(const std::string& script, const std::string& manifest) {
    Fixture fixture;
    std::array<oa::FeatureDef, 3> features{};
    std::snprintf(features[0].name, sizeof features[0].name, "Rock");
    std::snprintf(features[0].description, sizeof features[0].description, "a rock");
    features[0].dead_feature = 1;
    features[0].burnt_feature = static_cast<uint16_t>(0xffff);
    features[0].reclamate_feature = static_cast<uint16_t>(0xffff);
    features[0].metal = 1.5F;
    features[0].flags = OA_FEATURE_FLAG_RECLAIMABLE;
    std::snprintf(features[1].name, sizeof features[1].name, "Tree");
    features[1].dead_feature = static_cast<uint16_t>(0xffff);
    features[1].burnt_feature = static_cast<uint16_t>(0xffff);
    features[1].reclamate_feature = static_cast<uint16_t>(0xffff);
    std::snprintf(features[2].name, sizeof features[2].name, "Rock");
    features[2].dead_feature = static_cast<uint16_t>(0xffff);
    features[2].burnt_feature = static_cast<uint16_t>(0xffff);
    features[2].reclamate_feature = static_cast<uint16_t>(0xffff);
    fixture.match->adopt_feature_defs(features);

    auto& local = fixture.match->state().game.players[0];
    local.status = OA_PLAYER_STATUS_COMPUTER;
    local.in_use = 1;
    auto& machine = fixture.match->state().game.players[computer];
    machine.status = OA_PLAYER_STATUS_COMPUTER;
    machine.in_use = 1;
    seat_reader(fixture, script, manifest, computer);
    seat_reader(fixture, script, manifest, 0);

    auto& loaded = fixture.match->state().unit_defs[Type::vehicle];
    loaded.energy_use = 12.5F;
    loaded.flags |= OA_UNIT_DEF_FLAG_DOWNLOADABLE;
    std::snprintf(loaded.ai_weight, sizeof loaded.ai_weight, "ridge-weight");
    fixture.weapons.install_damage_override(1, "ARMSOLAR", 77);

    const auto& nuke = fixture.match->state().game.weapon_defs[1];
    const auto* weapon = fixture.match->weapon_definition(1);
    CHECK(weapon != nullptr);
    Published published;
    published.velocity_raw = 2 * 65536;
    published.energy_bits = bits_of(12.5F);
    published.energy_shot_bits = bits_of(nuke.energy_per_shot);
    published.metal_bits = bits_of(1.5F);
    published.nuke_reload = nuke.reload_time;
    published.damage_default = weapon->default_damage;
    published.damage_solar = 77;

    const auto own =
        brains::unit_ref(*fixture.match, fixture.spawn(computer, vehicle, 40, 40).unit_index);
    const auto foe = brains::unit_ref(*fixture.match, fixture.spawn(0, vehicle, 8, 8).unit_index);
    read_seat(fixture, *seat_for(fixture, computer), own, foe, published);
    read_seat(fixture, *seat_for(fixture, 0), foe, own, published);
}

std::string lower_copy(std::string_view text) {
    std::string out(text);
    for (char& letter : out) {
        const auto value = static_cast<unsigned char>(letter);
        if (value >= 'A' && value <= 'Z')
            letter = static_cast<char>(value - 'A' + 'a');
    }
    return out;
}

bool same_ci(std::string_view left, std::string_view right) {
    return lower_copy(left) == lower_copy(right);
}

bool text_matches(std::string_view stored, std::string_view file, std::size_t room) {
    if (room == 0)
        return stored == file;
    const std::size_t limit = room - 1;
    if (file.size() <= limit)
        return stored == file;
    return stored.size() == limit && file.substr(0, limit) == stored;
}

int64_t cast_width(int32_t value, schema::Storage storage) {
    switch (storage) {
    case schema::Storage::int8:
        return static_cast<int8_t>(value);
    case schema::Storage::uint8:
        return static_cast<uint8_t>(value);
    case schema::Storage::int16:
        return static_cast<int16_t>(value);
    case schema::Storage::uint16:
        return static_cast<uint16_t>(value);
    case schema::Storage::uint32:
        return static_cast<uint32_t>(value);
    case schema::Storage::ticks16:
        return static_cast<int16_t>(value);
    default:
        return value;
    }
}

std::size_t text_room(std::string_view key) {
    if (key == "name" || key == "unitname" || key == "objectname")
        return 32;
    if (key == "description" || key == "ai_weight" || key == "ai_limit")
        return 64;
    if (key == "side")
        return 30;
    return 0;
}

std::string_view chars_view(const char* text, std::size_t room) {
    std::size_t length = 0;
    if (text != nullptr) {
        while (length < room && text[length] != '\0')
            ++length;
    }
    return std::string_view(text == nullptr ? "" : text, length);
}

// Weapon 0 stands in for a name the weapon table does not hold. The typed
// definition keeps that stand-in as an empty name, the same way the unit
// definition does.
std::string weapon_kept(const defs::WeaponTable& weapons, oa_ref32 ref) {
    if (ref <= 1 || ref > OA_WEAPON_DEF_COUNT)
        return {};
    const char* key = weapons.defs[ref - 1].key;
    return std::string(chars_view(key, sizeof weapons.defs[ref - 1].key));
}

// A sound name that matches no category is stored as the number in the text,
// which is 0 when the text has no digits. The record then keeps that category's name.
std::string sound_kept(const defs::SoundCategoryTable& sounds, int16_t index) {
    if (index < 0 || static_cast<uint32_t>(index) >= sounds.count)
        return {};
    const char* name = sounds.categories[index].name;
    return std::string(chars_view(name, sizeof sounds.categories[index].name));
}

bool movement_key(std::string_view key) {
    return key == "footprintx" || key == "footprintz" || key == "minwaterdepth" ||
           key == "maxwaterdepth" || key == "maxslope" || key == "maxwaterslope";
}

int64_t movement_value(const oa::MoveClass& move, std::string_view key) {
    if (key == "footprintx")
        return move.footprint_x;
    if (key == "footprintz")
        return move.footprint_z;
    if (key == "minwaterdepth")
        return move.min_water_depth;
    if (key == "maxwaterdepth")
        return move.max_water_depth;
    if (key == "maxslope")
        return move.max_slope;
    return move.max_water_slope;
}

std::vector<std::string> category_tokens(const char* text) {
    std::vector<std::string> tokens;
    if (text == nullptr)
        return tokens;
    std::string token;
    for (const char* cursor = text;; ++cursor) {
        const bool end = *cursor == '\0';
        if (end || std::isspace(static_cast<unsigned char>(*cursor)) != 0) {
            if (!token.empty() && !same_ci(token, "ALL"))
                tokens.push_back(lower_copy(token));
            token.clear();
            if (end)
                break;
        } else {
            token.push_back(*cursor);
        }
    }
    std::sort(tokens.begin(), tokens.end());
    return tokens;
}

struct Tables {
    defs::MoveClassTable classes{};
    std::unique_ptr<defs::WeaponTable> weapons = std::make_unique<defs::WeaponTable>();
    defs::WeaponTdfSet weapon_files{};
    defs::SoundCategoryTable sounds{};
    defs::UnitDefTables units{};

    Tables() {
        defs::weapon_tdf_set_init(&weapon_files);
        defs::unit_def_tables_init(&units);
        defs::weapon_table_init(weapons.get());
    }

    ~Tables() {
        defs::weapon_table_free(weapons.get());
        defs::weapon_tdf_set_free(&weapon_files);
        defs::sound_category_table_free(&sounds);
        defs::unit_def_tables_free(&units);
    }

    Tables(const Tables&) = delete;
    Tables& operator=(const Tables&) = delete;
};

void compare_unit(
    const oa::UnitDef& record,
    const oa::data::unit_definitions::UnitDefinition& definition,
    const defs::MoveClassTable& classes,
    const defs::WeaponTable& weapons,
    const defs::SoundCategoryTable& sounds,
    const oa::formats::tdf::Block* block
) {
    const std::string unit(record.unit_name);
    check(
        record.min_water_depth == definition.min_water_depth &&
            record.max_water_depth == definition.max_water_depth,
        unit + ": water depth disagrees with the typed definition"
    );

    oa::MoveClass own{};
    defs::move_class_init_defaults(&own);
    const oa::MoveClass* move = &own;
    if (record.move_class == 0) {
        defs::move_class_load(&own, block);
    } else if (record.move_class <= OA_MOVE_CLASS_COUNT) {
        move = &classes.classes[record.move_class - 1];
    } else {
        fail(unit + ": movement class is outside the table");
        return;
    }

    const int32_t max_velocity = oa::formats::tdf::get_fixed(block, "maxvelocity", 0);
    const auto doubled = static_cast<int32_t>(static_cast<uint32_t>(max_velocity) << 1);
    const int32_t cloak = oa::formats::tdf::get_int(block, "cloakcost", 0);
    const auto cloak_whole = static_cast<int32_t>(static_cast<uint64_t>(
        oa::base::game_math::truncate_to_int64(static_cast<double>(static_cast<float>(cloak)))
    ));

    for (const auto& spec : schema::fields(schema::RecordKind::unit)) {
        if (brains::omitted_unit_reason(spec.key) != nullptr)
            continue;
        brains::FieldValue value;
        if (!brains::read_unit_field(record, &definition, spec.key, value)) {
            fail(unit + ": " + std::string(spec.key) + " is not read");
            continue;
        }
        const std::string key(spec.key);
        if (movement_key(spec.key)) {
            check(
                value.kind == brains::FieldValue::Kind::integer &&
                    value.integer == movement_value(*move, spec.key),
                unit + ": " + key + " disagrees with the movement class"
            );
            continue;
        }
        if (spec.type == schema::FieldType::flag) {
            const bool bit = (oa::formats::tdf::get_int(block, spec.key.data(), 0) & 1) != 0;
            check(
                value.kind == brains::FieldValue::Kind::boolean && value.boolean == bit,
                unit + ": " + key
            );
            continue;
        }
        if (spec.key == "category") {
            auto from_file = category_tokens(oa::formats::tdf::find_value(block, "category"));
            std::vector<std::string> from_record;
            if (value.listed != nullptr) {
                for (const auto& item : value.listed->categories)
                    from_record.push_back(lower_copy(item));
            }
            std::sort(from_record.begin(), from_record.end());
            check(
                value.kind == brains::FieldValue::Kind::list && from_file == from_record,
                unit + ": category"
            );
            continue;
        }
        if (spec.key == "defaultmissiontype") {
            const char* file = oa::formats::tdf::find_value(block, "defaultmissiontype");
            const auto index = mission::index_for_name(file == nullptr ? "" : file);
            const auto names = mission::registered_names();
            const std::string_view spelling =
                static_cast<std::size_t>(index) < names.size() ? names[index] : std::string_view{};
            check(
                value.kind == brains::FieldValue::Kind::text && same_ci(value.text, spelling),
                unit + ": defaultmissiontype"
            );
            continue;
        }
        if (spec.type == schema::FieldType::text || spec.type == schema::FieldType::name_ref) {
            const char* file = oa::formats::tdf::find_value(block, spec.key.data());
            std::string expected = file == nullptr ? std::string{} : file;
            if (file == nullptr && spec.key == "objectname")
                expected = std::string(chars_view(record.unit_name, sizeof record.unit_name));
            // objectname is a name reference in the schema and a truncated
            // buffer on the record. The other name references are the resolved
            // names, compared in full. explodeas, selfdestructas and
            // soundcategory keep a table entry, not the file token, when that
            // token names nothing the loader resolved.
            const bool truncated = spec.key == "objectname" || spec.type == schema::FieldType::text;
            if (!truncated && spec.key == "explodeas" &&
                !same_ci(weapon_kept(weapons, record.explode_as), expected))
                expected = weapon_kept(weapons, record.explode_as);
            if (!truncated && spec.key == "selfdestructas" &&
                !same_ci(weapon_kept(weapons, record.self_destruct_as), expected))
                expected = weapon_kept(weapons, record.self_destruct_as);
            if (!truncated && spec.key == "soundcategory" &&
                !same_ci(sound_kept(sounds, record.sound_category), expected))
                expected = sound_kept(sounds, record.sound_category);
            const bool matched = truncated ? text_matches(value.text, expected, text_room(spec.key))
                                           : same_ci(value.text, expected);
            check(
                value.kind == brains::FieldValue::Kind::text && matched,
                unit + ": " + key + " read '" + std::string(value.text) + "' expected '" +
                    expected + "'"
            );
            continue;
        }
        if (spec.storage == schema::Storage::fixed32) {
            int32_t fallback = 0;
            if (spec.key == "bankscale" || spec.key == "damagemodifier")
                fallback = defs::fixed_unit_scale;
            if (spec.key == "moverate1" || spec.key == "moverate2")
                fallback = doubled;
            const int32_t expected = oa::formats::tdf::get_fixed(block, spec.key.data(), fallback);
            check(
                value.kind == brains::FieldValue::Kind::fixed && value.integer == expected,
                unit + ": " + key
            );
            continue;
        }
        if (spec.storage == schema::Storage::float32) {
            const float expected =
                static_cast<float>(oa::formats::tdf::get_double(block, spec.key.data(), 0.0));
            check(
                value.kind == brains::FieldValue::Kind::real &&
                    bits_of(value.real) == bits_of(expected),
                unit + ": " + key
            );
            continue;
        }
        int64_t expected = 0;
        if (spec.key == "buildcostenergy" || spec.key == "buildcostmetal" ||
            spec.key == "cloakcost") {
            const auto raw = oa::formats::tdf::get_int(block, spec.key.data(), 0);
            expected = static_cast<int32_t>(static_cast<float>(raw));
        } else if (spec.key == "cloakcostmoving") {
            const auto raw = oa::formats::tdf::get_int(block, "cloakcostmoving", cloak_whole);
            expected = static_cast<int32_t>(static_cast<float>(raw));
        } else if (spec.key == "standingmoveorder" || spec.key == "standingfireorder") {
            const auto raw = static_cast<uint32_t>(oa::formats::tdf::get_int(
                block, spec.key.data(), static_cast<int32_t>(defs::standing_order_default)
            ));
            expected = raw & 3U;
        } else if (spec.key == "selfdestructcountdown") {
            const char* file = oa::formats::tdf::find_value(block, "selfdestructcountdown");
            const auto raw = file == nullptr
                                 ? defs::self_destruct_countdown_default
                                 : static_cast<uint32_t>(oa::formats::tdf::parse_int(file));
            expected = raw & 7U;
        } else if (spec.key == "mincloakdistance") {
            auto distance =
                static_cast<int16_t>(oa::formats::tdf::get_int(block, "mincloakdistance", 0));
            if (static_cast<float>(cloak) > 0.0F && distance == 0)
                distance = defs::min_cloak_distance_default;
            expected = distance;
        } else if (
            spec.storage == schema::Storage::ticks16 || spec.storage == schema::Storage::ticks32
        ) {
            const double seconds = oa::formats::tdf::get_double(block, spec.key.data(), 0.0);
            const auto ticks = static_cast<int32_t>(seconds * 30.0);
            expected = cast_width(ticks, spec.storage);
        } else {
            expected =
                cast_width(oa::formats::tdf::get_int(block, spec.key.data(), 0), spec.storage);
        }
        check(
            value.kind == brains::FieldValue::Kind::integer && value.integer == expected,
            unit + ": " + key
        );
    }
}

void check_installed(const oa::AssetStore& assets) {
    const auto files = defs::asset_store_files(&assets);
    Tables tables;
    if (!defs::load_move_classes(&files, &tables.classes, nullptr, nullptr) ||
        defs::load_weapon_defs(&files, tables.weapons.get(), nullptr) == 0 ||
        !defs::load_weapon_tdf_set(&files, nullptr, false, &tables.weapon_files) ||
        !defs::load_sound_categories(&files, &tables.sounds, nullptr)) {
        fail("the install's movement classes, weapons or sound categories do not load");
        return;
    }
    std::vector<std::string> names;
    files.list(
        files.context,
        "units",
        "FBI",
        [](void* user, const char* name) {
            static_cast<std::vector<std::string>*>(user)->emplace_back(name);
        },
        &names
    );
    if (names.empty() ||
        !defs::unit_def_tables_allocate(&tables.units, static_cast<uint32_t>(names.size() + 1))) {
        fail("the install holds no units");
        return;
    }
    const defs::UnitHeaderSources header_sources{"", &tables.weapon_files, 3, 1, false, false};
    for (std::size_t index = 0; index < names.size(); ++index) {
        char path[defs::path_capacity];
        defs::build_variant_path(
            &files, path, sizeof path, "units", names[index].c_str(), "FBI", nullptr
        );
        bool refused = false;
        if (!defs::load_unit_header(
                &files, path, tables.units.records[index + 1], header_sources, &refused
            ))
            fail(std::string(path) + ": the header does not load");
    }
    tables.units.count = defs::unit_defs_finalize_catalog(tables.units.records, tables.units.count);
    const defs::UnitDefSources sources{
        "",
        &tables.classes,
        tables.weapons->defs,
        &tables.sounds,
        &tables.units.categories,
        &tables.units.blocks,
        nullptr
    };
    std::vector<oa::data::unit_definitions::UnitDefinition> definitions(tables.units.count);
    bool saw_commander = false;
    for (uint32_t type = 1; type < tables.units.count; ++type) {
        auto& record = tables.units.records[type];
        char path[defs::path_capacity];
        defs::build_variant_path(
            &files, path, sizeof path, "units", record.unit_name, "FBI", nullptr
        );
        if (!defs::load_unit_def(&files, path, record, sources)) {
            fail(std::string(record.unit_name) + ": the unit does not load");
            continue;
        }
        definitions[type] = oa::data::unit_definitions::unit_definition_from(
            record,
            {&tables.classes, tables.weapons->defs, &tables.sounds, &tables.units.categories}
        );
        oa::formats::tdf::Document document;
        oa::formats::tdf::document_init(&document);
        if (!defs::load_tdf_file(&files, path, &document, nullptr)) {
            oa::formats::tdf::document_free(&document);
            fail(std::string(record.unit_name) + ": the unit text does not parse");
            continue;
        }
        const auto* block = oa::formats::tdf::find_child(document.root, "UNITINFO");
        if (block == nullptr) {
            oa::formats::tdf::document_free(&document);
            fail(std::string(record.unit_name) + ": no unit block");
            continue;
        }
        compare_unit(
            record, definitions[type], tables.classes, *tables.weapons, tables.sounds, block
        );
        oa::formats::tdf::document_free(&document);
        if (oa::formats::tdf::compare_nocase(record.unit_name, "ARMCOM") == 0) {
            saw_commander = true;
            brains::FieldValue commander;
            brains::FieldValue side;
            check(
                brains::read_unit_field(record, &definitions[type], "commander", commander) &&
                    commander.kind == brains::FieldValue::Kind::boolean && commander.boolean,
                "ARMCOM commander"
            );
            check(
                brains::read_unit_field(record, &definitions[type], "side", side) &&
                    side.kind == brains::FieldValue::Kind::text && same_ci(side.text, "ARM"),
                "ARMCOM side"
            );
        }
    }
    check(saw_commander, "no ARMCOM");
}

} // namespace

int main(int argc, char** argv) {
    bool data = false;
    for (int index = 1; index < argc; ++index) {
        if (argv[index] != nullptr && std::strcmp(argv[index], "--data") == 0)
            data = true;
    }
    if (data) {
        check_installed(oa::test::require_game_assets("the installed unit definitions"));
        return failures == 0 ? 0 : 1;
    }
    require_schema();
    const std::string script = load_text("brain.lua");
    const std::string manifest = load_text("oabrain.yaml");
    check_outside(script, manifest);
    check_seated(script, manifest);
    return 0;
}
