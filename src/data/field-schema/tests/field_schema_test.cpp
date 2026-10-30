// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The schema's keys, ranges and defaults, and a pass over the installed game.
#include "oa/data/field_schema.hpp"

#include "oa/test/check.hpp"
#include "oa/test/game_assets.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The lists below are the Context's keys. Unit: unit_def_loader.cpp, unit_header.cpp
// and move_classes.cpp. Weapon: weapons.cpp, including shellweapon from the flag
// table. Movement class: move_classes.cpp.
#include "key_lists.inc"

namespace {

using oa::data::field_schema::Check;
using oa::data::field_schema::DefaultRule;
using oa::data::field_schema::FieldType;
using oa::data::field_schema::RecordKind;
using oa::data::field_schema::Storage;
using oa::data::field_schema::Value;
using oa::data::field_schema::Verdict;
using oa::data::field_schema::check_number;
using oa::data::field_schema::check_text;
using oa::data::field_schema::default_value;
using oa::data::field_schema::fields;
using oa::data::field_schema::find_field;
using oa::data::field_schema::read_value;

bool listed(const char* const* keys, std::size_t count, std::string_view key) {
    for (std::size_t index = 0; index < count; ++index)
        if (key == keys[index])
            return true;
    return false;
}

void expect_keys(RecordKind record, const char* const* keys, std::size_t count) {
    OA_CHECK(fields(record).size() == count);
    for (std::size_t index = 0; index < count; ++index) {
        const auto* spec = find_field(record, keys[index]);
        OA_CHECK(spec != nullptr);
        OA_CHECK(spec->key == keys[index]);
        std::size_t matches = 0;
        for (const auto& field : fields(record))
            if (field.key == keys[index])
                ++matches;
        OA_CHECK(matches == 1);
    }
    for (const auto& field : fields(record))
        OA_CHECK(listed(keys, count, field.key));
}

void expect_verdict(Verdict verdict, Check result) {
    OA_CHECK(result.verdict == verdict);
    OA_CHECK((verdict == Verdict::ok) == result.reason.empty());
}

const oa::formats::tdf::Block* section_of(oa::formats::tdf::Document* document, const char* text) {
    oa::formats::tdf::document_init(document);
    const auto length = static_cast<uint32_t>(std::strlen(text));
    OA_CHECK(oa::formats::tdf::parse_text(document, text, length, false, nullptr));
    OA_CHECK(oa::formats::tdf::select_section(document, "U"));
    return oa::formats::tdf::cursor(document);
}

void check_schema() {
    expect_keys(RecordKind::unit, k_unit_keys, std::size(k_unit_keys));
    expect_keys(RecordKind::weapon, k_weapon_keys, std::size(k_weapon_keys));
    expect_keys(RecordKind::move_class, k_move_keys, std::size(k_move_keys));
    OA_CHECK(std::size(k_unit_keys) == 120);
    OA_CHECK(std::size(k_weapon_keys) == 73);
    OA_CHECK(std::size(k_move_keys) == 9);
    OA_CHECK(fields(RecordKind::weapon_damage).size() == 2);

    const auto* velocity = find_field(RecordKind::unit, "maxvelocity");
    const auto* energy = find_field(RecordKind::unit, "energymake");
    const auto* turn = find_field(RecordKind::unit, "turnrate");
    const auto* fly = find_field(RecordKind::unit, "canfly");
    const auto* weapon = find_field(RecordKind::unit, "weapon1");
    const auto* category = find_field(RecordKind::unit, "category");
    const auto* yard = find_field(RecordKind::unit, "yardmap");
    const auto* reload = find_field(RecordKind::weapon, "reloadtime");
    const auto* ident = find_field(RecordKind::weapon, "id");
    OA_CHECK(velocity->type == FieldType::fixed && velocity->storage == Storage::fixed32);
    OA_CHECK(energy->type == FieldType::decimal && energy->storage == Storage::float32);
    OA_CHECK(turn->type == FieldType::integer && turn->storage == Storage::int16);
    OA_CHECK(fly->type == FieldType::flag && fly->storage == Storage::none);
    OA_CHECK(weapon->type == FieldType::name_ref);
    OA_CHECK(category->type == FieldType::token_list);
    OA_CHECK(yard->type == FieldType::yard_map);
    OA_CHECK(reload->type == FieldType::decimal && reload->storage == Storage::ticks16);
    OA_CHECK(ident->type == FieldType::integer);
    OA_CHECK(find_field(RecordKind::weapon, "color")->storage == Storage::uint8);
    OA_CHECK(find_field(RecordKind::weapon, "color2")->storage == Storage::uint8);
    OA_CHECK(find_field(RecordKind::weapon, "tolerance")->storage == Storage::uint16);
    OA_CHECK(find_field(RecordKind::unit, "buildangle")->storage == Storage::uint16);
    OA_CHECK(find_field(RecordKind::weapon, "shellweapon")->type == FieldType::flag);
    OA_CHECK(find_field(RecordKind::weapon, "turnrate")->type == FieldType::decimal);

    const auto nan = std::numeric_limits<double>::quiet_NaN();
    const auto inf = std::numeric_limits<double>::infinity();
    expect_verdict(Verdict::refuse, check_number(RecordKind::unit, "maxvelocity", nan));
    expect_verdict(Verdict::refuse, check_number(RecordKind::unit, "maxvelocity", inf));
    expect_verdict(Verdict::refuse, check_number(RecordKind::unit, "maxvelocity", -inf));
    expect_verdict(Verdict::refuse, check_number(RecordKind::unit, "footprintx", 0));
    expect_verdict(Verdict::refuse, check_number(RecordKind::unit, "footprintx", -1));
    expect_verdict(Verdict::refuse, check_number(RecordKind::unit, "footprintx", 1025));
    expect_verdict(Verdict::ok, check_number(RecordKind::unit, "footprintx", 1));
    expect_verdict(Verdict::ok, check_number(RecordKind::unit, "footprintx", 1024));
    expect_verdict(Verdict::refuse, check_number(RecordKind::move_class, "footprintz", 0));
    expect_verdict(Verdict::refuse, check_number(RecordKind::unit, "transportcapacity", -1));
    expect_verdict(Verdict::refuse, check_number(RecordKind::unit, "transportsize", -1));
    expect_verdict(Verdict::refuse, check_number(RecordKind::weapon, "id", 256));
    expect_verdict(Verdict::refuse, check_number(RecordKind::weapon, "id", -1));
    expect_verdict(Verdict::ok, check_number(RecordKind::weapon, "id", 0));
    expect_verdict(Verdict::ok, check_number(RecordKind::weapon, "id", 255));
    expect_verdict(Verdict::ok, check_number(RecordKind::weapon, "id", 1023, 1024));
    expect_verdict(Verdict::refuse, check_number(RecordKind::weapon, "id", 1024, 1024));
    OA_CHECK(
        check_number(RecordKind::weapon, "id", 1024, 1024).reason.find("1023") != std::string::npos
    );
    expect_verdict(Verdict::ok, check_text(RecordKind::weapon, "id", "1023", 1024));
    expect_verdict(Verdict::refuse, check_text(RecordKind::weapon, "id", "1024", 1024));
    OA_CHECK(
        check_text(RecordKind::weapon, "id", "1024", 1024).reason.find("1023") != std::string::npos
    );
    expect_verdict(Verdict::warn, check_number(RecordKind::weapon, "color", 256, 1024));
    expect_verdict(Verdict::refuse, check_number(RecordKind::weapon, "burst", -1));
    expect_verdict(Verdict::refuse, check_text(RecordKind::unit, "name", "a;b"));
    expect_verdict(Verdict::refuse, check_text(RecordKind::unit, "name", "a\nb"));
    expect_verdict(
        Verdict::refuse, check_text(RecordKind::unit, "description", std::string_view("a\0b", 3))
    );
    expect_verdict(Verdict::refuse, check_text(RecordKind::unit, "name", "http://ridge"));
    expect_verdict(Verdict::refuse, check_text(RecordKind::unit, "name", "a/*b"));
    expect_verdict(Verdict::refuse, check_text(RecordKind::unit, "footprintx", "abc"));
    OA_CHECK(
        check_text(RecordKind::unit, "footprintx", "abc").reason.find("is not a number") !=
        std::string::npos
    );
    expect_verdict(Verdict::ok, check_text(RecordKind::unit, "tedclass", "a;b"));

    // footprintx is an integer, maxvelocity a fixed number, canfly a flag.
    // A text field (name, description) is unchanged by these samples.
    const auto not_number = [](RecordKind record, const char* key, const char* text) {
        const Check result = check_text(record, key, text);
        OA_CHECK(result.verdict == Verdict::refuse);
        OA_CHECK(result.reason.find("is not a number") != std::string::npos);
    };
    const auto same = [](RecordKind record, const char* key, const char* text, double number) {
        OA_CHECK(
            check_text(record, key, text).verdict == check_number(record, key, number).verdict
        );
    };
    for (const char* sample : {"abc", "", " ", "12abc", "-5", "1e3", "0x10"}) {
        expect_verdict(Verdict::ok, check_text(RecordKind::unit, "name", sample));
        expect_verdict(Verdict::ok, check_text(RecordKind::unit, "description", sample));
    }
    for (const char* key : {"footprintx", "maxvelocity", "canfly"}) {
        not_number(RecordKind::unit, key, "abc");
        not_number(RecordKind::unit, key, "");
        not_number(RecordKind::unit, key, " ");
    }
    expect_verdict(Verdict::warn, check_text(RecordKind::unit, "footprintx", "12abc"));
    expect_verdict(Verdict::warn, check_text(RecordKind::unit, "maxvelocity", "12abc"));
    expect_verdict(Verdict::warn, check_text(RecordKind::unit, "canfly", "12abc"));
    OA_CHECK(
        check_text(RecordKind::unit, "maxvelocity", "12abc").reason.find("abc") != std::string::npos
    );
    same(RecordKind::unit, "footprintx", "-5", -5);
    same(RecordKind::unit, "maxvelocity", "-5", -5);
    same(RecordKind::unit, "canfly", "-5", -5);
    // An integer stops at 'e'; a fixed field reads the exponent.
    expect_verdict(Verdict::warn, check_text(RecordKind::unit, "footprintx", "1e3"));
    OA_CHECK(
        check_text(RecordKind::unit, "footprintx", "1e3").reason.find("e3") != std::string::npos
    );
    same(RecordKind::unit, "maxvelocity", "1e3", 1000);
    expect_verdict(Verdict::warn, check_text(RecordKind::unit, "canfly", "1e3"));
    // "0x10" is the digit 0. footprintx 0 is refused; the others only warn
    // about the letters the loader skips.
    same(RecordKind::unit, "footprintx", "0x10", 0);
    expect_verdict(Verdict::warn, check_text(RecordKind::unit, "maxvelocity", "0x10"));
    expect_verdict(Verdict::warn, check_text(RecordKind::unit, "canfly", "0x10"));
    OA_CHECK(
        check_text(RecordKind::unit, "maxvelocity", "0x10").reason.find("x10") != std::string::npos
    );

    expect_verdict(Verdict::warn, check_number(RecordKind::unit, "maxdamage", 32768));
    expect_verdict(Verdict::warn, check_number(RecordKind::unit, "maxdamage", 0));
    expect_verdict(Verdict::ok, check_number(RecordKind::unit, "maxdamage", 32767));
    expect_verdict(Verdict::ok, check_number(RecordKind::unit, "maxdamage", 1));
    expect_verdict(Verdict::warn, check_number(RecordKind::unit, "turnrate", 40000));
    expect_verdict(Verdict::ok, check_number(RecordKind::weapon, "color", 144));
    expect_verdict(Verdict::ok, check_number(RecordKind::weapon, "color", 255));
    expect_verdict(Verdict::warn, check_number(RecordKind::weapon, "color", 256));
    expect_verdict(Verdict::warn, check_number(RecordKind::weapon, "color", -1));
    expect_verdict(Verdict::ok, check_number(RecordKind::weapon, "color2", 144));
    expect_verdict(Verdict::ok, check_number(RecordKind::weapon, "color2", 255));
    expect_verdict(Verdict::warn, check_number(RecordKind::weapon, "color2", 256));
    expect_verdict(Verdict::warn, check_number(RecordKind::weapon, "color2", -1));
    expect_verdict(Verdict::ok, check_number(RecordKind::weapon, "tolerance", 37152));
    expect_verdict(Verdict::ok, check_number(RecordKind::weapon, "tolerance", 65535));
    expect_verdict(Verdict::warn, check_number(RecordKind::weapon, "tolerance", 65536));
    expect_verdict(Verdict::warn, check_number(RecordKind::weapon, "tolerance", -1));
    expect_verdict(Verdict::ok, check_number(RecordKind::unit, "buildangle", 32768));
    expect_verdict(Verdict::warn, check_number(RecordKind::unit, "buildangle", 65536));
    expect_verdict(Verdict::warn, check_number(RecordKind::unit, "workertime", 32768));
    expect_verdict(Verdict::warn, check_number(RecordKind::unit, "buildtime", 600001));
    expect_verdict(Verdict::ok, check_number(RecordKind::unit, "buildtime", 600000));
    expect_verdict(Verdict::warn, check_number(RecordKind::weapon, "range", 32001));
    expect_verdict(Verdict::ok, check_number(RecordKind::weapon, "range", 32000));
    expect_verdict(Verdict::warn, check_number(RecordKind::weapon_damage, "default", 40000));
    expect_verdict(Verdict::warn, check_number(RecordKind::weapon_damage, "ridgeunit", 40000));
    expect_verdict(Verdict::ok, check_number(RecordKind::weapon_damage, "default", 32767));
    expect_verdict(Verdict::warn, check_number(RecordKind::weapon, "reloadtime", 1100));
    expect_verdict(Verdict::warn, check_number(RecordKind::unit, "canfly", 2));
    expect_verdict(Verdict::ok, check_number(RecordKind::unit, "canfly", 0));
    expect_verdict(Verdict::ok, check_number(RecordKind::unit, "canfly", 1));
    expect_verdict(Verdict::warn, check_number(RecordKind::unit, "bmcode", 3));
    expect_verdict(Verdict::warn, check_number(RecordKind::unit, "buildcostmetal", 3000000000.0));
    expect_verdict(Verdict::warn, check_number(RecordKind::unit, "maxvelocity", 40000));
    expect_verdict(Verdict::ok, check_number(RecordKind::unit, "workertime", 67.5));
    expect_verdict(Verdict::ok, check_number(RecordKind::unit, "buildcostmetal", 412.5));
    expect_verdict(Verdict::ok, check_text(RecordKind::unit, "workertime", "67.5"));
    expect_verdict(Verdict::ok, check_text(RecordKind::unit, "buildcostmetal", "412.5"));

    const auto agree = [](RecordKind record, const char* key, const char* text, double number) {
        OA_CHECK(
            check_text(record, key, text).verdict == check_number(record, key, number).verdict
        );
    };
    agree(RecordKind::unit, "workertime", "67.5", 67.5);
    agree(RecordKind::unit, "buildcostmetal", "412.5", 412.5);
    agree(RecordKind::unit, "turnrate", "40000", 40000);
    agree(RecordKind::unit, "maxdamage", "32767", 32767);
    agree(RecordKind::unit, "maxdamage", "32768", 32768);
    agree(RecordKind::unit, "buildtime", "600000", 600000);
    agree(RecordKind::unit, "buildtime", "600001", 600001);
    agree(RecordKind::weapon, "range", "32000", 32000);
    agree(RecordKind::weapon, "range", "32001", 32001);
    agree(RecordKind::weapon, "id", "255", 255);
    agree(RecordKind::weapon, "id", "256", 256);
    agree(RecordKind::unit, "footprintx", "1", 1);
    agree(RecordKind::unit, "footprintx", "1024", 1024);
    agree(RecordKind::unit, "footprintx", "0", 0);
    agree(RecordKind::unit, "canfly", "2", 2);
    agree(RecordKind::unit, "bmcode", "3", 3);
    agree(RecordKind::weapon, "reloadtime", "1100", 1100);
    agree(RecordKind::unit, "maxvelocity", "40000", 40000);
    agree(RecordKind::unit, "buildcostmetal", "3000000000", 3000000000.0);
    agree(RecordKind::weapon, "reloadtime", "1e999", inf);

    OA_CHECK(find_field(RecordKind::unit, "tedclass") == nullptr);
    OA_CHECK(find_field(RecordKind::unit, "ridge_custom_tag") == nullptr);
    expect_verdict(Verdict::ok, check_text(RecordKind::unit, "tedclass", "builder"));
    expect_verdict(Verdict::ok, check_text(RecordKind::weapon, "ridge_custom_tag", "1"));
    expect_verdict(Verdict::ok, check_number(RecordKind::unit, "tedclass", nan));
    OA_CHECK(find_field(RecordKind::unit, "MaxVelocity") == velocity);
    expect_verdict(Verdict::warn, check_text(RecordKind::unit, "CanFly", "2"));
    OA_CHECK(find_field(RecordKind::weapon_damage, "default")->storage == Storage::int16);
    OA_CHECK(find_field(RecordKind::weapon_damage, "ARMCOM")->key.empty());
    OA_CHECK(find_field(RecordKind::weapon_damage, "ARMCOM")->storage == Storage::int32);
    OA_CHECK(find_field(RecordKind::weapon_damage, "Default")->storage == Storage::int16);

    const Value turned = read_value(turn, "40000");
    OA_CHECK(turned.kind == Value::Kind::integer);
    OA_CHECK(turned.integer == -25536);
    const Value tolerated = read_value(find_field(RecordKind::weapon, "tolerance"), "37152");
    OA_CHECK(tolerated.kind == Value::Kind::integer && tolerated.integer == 37152);
    const Value coloured = read_value(find_field(RecordKind::weapon, "color"), "200");
    OA_CHECK(coloured.kind == Value::Kind::integer && coloured.integer == 200);
    const Value angled = read_value(find_field(RecordKind::unit, "buildangle"), "32768");
    OA_CHECK(angled.kind == Value::Kind::integer && angled.integer == 32768);
    const Value flown = read_value(fly, "2");
    OA_CHECK(flown.kind == Value::Kind::boolean && !flown.boolean);
    const Value paced = read_value(velocity, "1.6");
    OA_CHECK(paced.kind == Value::Kind::number && paced.number == 1.6);
    OA_CHECK(read_value(turn, nullptr).kind == Value::Kind::absent);
    const Value blank = read_value(find_field(RecordKind::unit, "footprintx"), "abc");
    OA_CHECK(blank.kind == Value::Kind::integer && blank.integer == 0 && blank.number == 0.0);
    const Value unknown = read_value(nullptr, "ridge");
    OA_CHECK(unknown.kind == Value::Kind::text && unknown.text == "ridge");

    OA_CHECK(default_value(RecordKind::unit, "bankscale", nullptr).number == 1.0);
    OA_CHECK(default_value(RecordKind::unit, "damagemodifier", nullptr).number == 1.0);
    OA_CHECK(default_value(RecordKind::unit, "canfly", nullptr).kind == Value::Kind::boolean);
    OA_CHECK(!default_value(RecordKind::unit, "canfly", nullptr).boolean);
    OA_CHECK(default_value(RecordKind::unit, "energyuse", nullptr).number == 0.0);
    OA_CHECK(default_value(RecordKind::unit, "description", nullptr).text.empty());
    OA_CHECK(default_value(RecordKind::unit, "nochasecategory", nullptr).text == "none");
    OA_CHECK(default_value(RecordKind::unit, "standingfireorder", nullptr).integer == 2);
    OA_CHECK(default_value(RecordKind::unit, "selfdestructcountdown", nullptr).integer == 5);
    OA_CHECK(default_value(RecordKind::weapon, "id", nullptr).integer == -1);
    OA_CHECK(default_value(RecordKind::weapon, "range", nullptr).integer == 32767);
    OA_CHECK(default_value(RecordKind::weapon, "minbarrelangle", nullptr).number == -11.25);
    OA_CHECK(default_value(RecordKind::unit, "tedclass", nullptr).kind == Value::Kind::absent);
    OA_CHECK(default_value(RecordKind::move_class, "maxslope", nullptr).integer == 255);
    OA_CHECK(default_value(RecordKind::move_class, "badslope", nullptr).integer == 127);
    OA_CHECK(default_value(RecordKind::unit, "maxslope", nullptr).integer == 255);
    OA_CHECK(default_value(RecordKind::unit, "badslope", nullptr).integer == 127);
    OA_CHECK(default_value(RecordKind::unit, "mincloakdistance", nullptr).integer == 0);
    OA_CHECK(default_value(RecordKind::unit, "moverate1", nullptr).number == 0.0);
    OA_CHECK(default_value(RecordKind::weapon_damage, "ridgeunit", nullptr).integer == 0);

    oa::formats::tdf::Document fast;
    const auto* fast_block = section_of(&fast, "[U]{maxvelocity=1.5;}");
    OA_CHECK(default_value(RecordKind::unit, "moverate1", fast_block).number == 3.0);
    OA_CHECK(default_value(RecordKind::unit, "moverate2", fast_block).number == 3.0);
    oa::formats::tdf::document_free(&fast);

    oa::formats::tdf::Document cloak;
    const auto* cloak_block = section_of(&cloak, "[U]{cloakcost=40;}");
    OA_CHECK(default_value(RecordKind::unit, "cloakcostmoving", cloak_block).integer == 40);
    OA_CHECK(default_value(RecordKind::unit, "mincloakdistance", cloak_block).integer == 80);
    oa::formats::tdf::document_free(&cloak);

    oa::formats::tdf::Document named;
    const auto* named_block = section_of(&named, "[U]{unitname=RIDGETANK;}");
    OA_CHECK(default_value(RecordKind::unit, "objectname", named_block).text == "RIDGETANK");
    oa::formats::tdf::document_free(&named);

    oa::formats::tdf::Document slope;
    const auto* slope_block = section_of(&slope, "[U]{maxslope=30;}");
    OA_CHECK(default_value(RecordKind::unit, "badslope", slope_block).integer == 15);
    OA_CHECK(default_value(RecordKind::move_class, "badslope", slope_block).integer == 15);
    oa::formats::tdf::document_free(&slope);

    oa::formats::tdf::Document water;
    const auto* water_block = section_of(&water, "[U]{maxwaterslope=50;}");
    OA_CHECK(default_value(RecordKind::unit, "badwaterslope", water_block).integer == 25);
    oa::formats::tdf::document_free(&water);

    OA_CHECK(find_field(RecordKind::unit, "moverate1")->default_rule == DefaultRule::twice_fixed);
    OA_CHECK(find_field(RecordKind::unit, "bankscale")->default_number == 1.0);
    OA_CHECK(find_field(RecordKind::unit, "nochasecategory")->default_text == "none");
}

std::string canonical_key(RecordKind record, std::string_view key) {
    const auto* spec = find_field(record, key);
    if (spec == nullptr || spec->key.empty()) {
        std::string folded(key);
        for (char& character : folded)
            if (character >= 'A' && character <= 'Z')
                character = static_cast<char>(character + 32);
        return folded;
    }
    return std::string(spec->key);
}

void check_block(
    RecordKind record,
    const oa::formats::tdf::Block* block,
    const std::string& path,
    std::set<std::pair<std::string, std::string>>* warnings,
    int* keys,
    bool* refused
) {
    if (block == nullptr)
        return;
    for (uint32_t index = 0; index < block->property_count; ++index) {
        const char* key = oa::formats::tdf::property_key_at(block, static_cast<int32_t>(index));
        const char* value = key != nullptr ? oa::formats::tdf::find_value(block, key) : nullptr;
        if (key == nullptr || value == nullptr)
            continue;
        ++*keys;
        const Check result = check_text(record, key, value);
        if (result.verdict == Verdict::refuse) {
            *refused = true;
            std::printf("REFUSE %s %s: %s\n", path.c_str(), key, result.reason.c_str());
        } else if (result.verdict == Verdict::warn) {
            warnings->emplace(canonical_key(record, key), value);
        }
    }
}

int run_game_data() {
    const auto assets = oa::test::require_game_assets("unit, weapon and movement field schema");
    const auto unit_paths = assets.list_effective("units", ".fbi");
    const auto weapon_paths = assets.list_effective("weapons", ".tdf");
    std::string move_path;
    for (const std::string& path : assets.list_effective("gamedata", ".tdf"))
        if (path.size() >= std::string_view("moveinfo.tdf").size() &&
            path.ends_with("moveinfo.tdf"))
            move_path = path;
    if (unit_paths.empty() || weapon_paths.empty() || move_path.empty()) {
        std::printf(
            "FAIL field schema: the installed game listed %zu unit files, %zu weapon files, "
            "moveinfo %s\n",
            unit_paths.size(),
            weapon_paths.size(),
            move_path.empty() ? "missing" : "present"
        );
        return 1;
    }

    std::set<std::pair<std::string, std::string>> warnings;
    int file_count = 0;
    int key_count = 0;
    bool refused = false;
    const auto read_document = [&](const std::string& path, oa::formats::tdf::Document* document) {
        const auto bytes = oa::test::read_game_file(assets, path);
        oa::formats::tdf::document_init(document);
        oa::formats::tdf::ParseError error;
        if (bytes.empty() || !oa::formats::tdf::parse_text(
                                 document,
                                 reinterpret_cast<const char*>(bytes.data()),
                                 static_cast<uint32_t>(bytes.size()),
                                 false,
                                 &error
                             )) {
            std::printf("REFUSE %s: the file did not parse\n", path.c_str());
            refused = true;
            return false;
        }
        ++file_count;
        return true;
    };

    for (const std::string& path : unit_paths) {
        oa::formats::tdf::Document document;
        if (!read_document(path, &document)) {
            oa::formats::tdf::document_free(&document);
            continue;
        }
        if (!oa::formats::tdf::select_section(&document, "UNITINFO")) {
            std::printf("REFUSE %s: no UNITINFO section\n", path.c_str());
            refused = true;
        } else {
            check_block(
                RecordKind::unit,
                oa::formats::tdf::cursor(&document),
                path,
                &warnings,
                &key_count,
                &refused
            );
        }
        oa::formats::tdf::document_free(&document);
    }

    for (const std::string& path : weapon_paths) {
        oa::formats::tdf::Document document;
        if (!read_document(path, &document)) {
            oa::formats::tdf::document_free(&document);
            continue;
        }
        const oa::formats::tdf::Block* root = document.root;
        const uint32_t children = root != nullptr ? root->child_count : 0;
        for (uint32_t index = 0; index < children; ++index) {
            const oa::formats::tdf::Block* section = root->children[index];
            check_block(RecordKind::weapon, section, path, &warnings, &key_count, &refused);
            check_block(
                RecordKind::weapon_damage,
                oa::formats::tdf::find_child(section, "DAMAGE"),
                path,
                &warnings,
                &key_count,
                &refused
            );
        }
        oa::formats::tdf::document_free(&document);
    }

    oa::formats::tdf::Document moveinfo;
    if (!read_document(move_path, &moveinfo)) {
        oa::formats::tdf::document_free(&moveinfo);
    } else {
        const oa::formats::tdf::Block* root = moveinfo.root;
        const uint32_t children = root != nullptr ? root->child_count : 0;
        for (uint32_t index = 0; index < children; ++index)
            check_block(
                RecordKind::move_class,
                root->children[index],
                move_path,
                &warnings,
                &key_count,
                &refused
            );
        oa::formats::tdf::document_free(&moveinfo);
    }

    std::printf("files %d\nkeys %d\nwarnings %zu\n", file_count, key_count, warnings.size());
    bool unsigned_key_warned = false;
    for (const auto& warning : warnings) {
        std::printf("warning %s %s\n", warning.first.c_str(), warning.second.c_str());
        const std::string& name = warning.first;
        if (name == "buildangle" || name == "color" || name == "color2" || name == "tolerance")
            unsigned_key_warned = true;
    }
    OA_CHECK(!unsigned_key_warned);
    if (oa::test::failed_checks() != 0)
        return oa::test::check_exit_status();
    return refused ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
    if (oa::test::game_data_requested(argc, argv))
        return run_game_data();
    check_schema();
    return oa::test::check_exit_status();
}
