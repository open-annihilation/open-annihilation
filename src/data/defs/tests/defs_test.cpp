// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/data/defs/categories.hpp"
#include "oa/data/defs/files.hpp"
#include "oa/data/defs/gamedata_tables.hpp"
#include "oa/data/defs/locale.hpp"
#include "oa/data/defs/move_classes.hpp"
#include "oa/data/defs/palette.hpp"
#include "oa/data/defs/sides.hpp"
#include "oa/data/defs/sound_categories.hpp"
#include "oa/data/defs/unit_catalog.hpp"
#include "oa/data/defs/unit_def_loader.hpp"
#include "oa/data/defs/unit_header.hpp"
#include "oa/data/defs/unit_records.hpp"
#include "oa/data/defs/unit_texts.hpp"
#include "oa/data/defs/version.hpp"
#include "oa/data/defs/weapons.hpp"

#include "oa/data/mission_types.hpp"
#include "test_files.hpp"
#include "oa/base/text.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace oa;
using namespace oa::data::defs;

namespace {

int failures = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::fprintf(                                                                          \
                stderr, "%s:%d: %s: CHECK(%s) failed\n", __FILE__, __LINE__, __func__, #condition  \
            );                                                                                     \
            ++failures;                                                                            \
        }                                                                                          \
    } while (false)

struct Doc {
    formats::tdf::Document document{};

    explicit Doc(const char* text) {
        formats::tdf::document_init(&document);
        formats::tdf::ParseError error{};
        if (!formats::tdf::parse_text(
                &document, text, static_cast<uint32_t>(std::strlen(text)), true, &error
            ))
            std::fprintf(
                stderr,
                "fixture failed to parse: %s\n",
                formats::tdf::parse_status_message(error.status)
            );
    }

    ~Doc() { formats::tdf::document_free(&document); }

    Doc(const Doc&) = delete;
    Doc& operator=(const Doc&) = delete;
};

uint32_t abilities_of(const UnitDef& record) {
    uint32_t value;
    std::memcpy(
        &value, reinterpret_cast<const unsigned char*>(&record) + offsetof(UnitDef, abilities), 4
    );
    return value;
}

void set_abilities(UnitDef& record, uint32_t value) {
    std::memcpy(
        reinterpret_cast<unsigned char*>(&record) + offsetof(UnitDef, abilities), &value, 4
    );
}

void set_flags(UnitDef& record, uint32_t value) {
    std::memcpy(reinterpret_cast<unsigned char*>(&record) + offsetof(UnitDef, flags), &value, 4);
}

uint32_t flags_of(const WeaponDef& weapon) {
    uint32_t value;
    std::memcpy(
        &value, reinterpret_cast<const unsigned char*>(&weapon) + offsetof(WeaponDef, flags), 4
    );
    return value;
}

void move_class_bad_slope_defaults_and_clamps() {
    Doc moveinfo(
        "[CLASS0]{name=TANKSH2; footprintx=2; footprintz=2; maxwaterdepth=22; maxslope=40;}"
        "[CLASS3]{Name=BOAT; maxslope=200; maxwaterslope=100; badwaterslope=180; minwaterdepth=15;}"
        "[CLASS4]{maxslope=10; badslope=90;}"
    );
    MoveClassTable table;
    move_class_table_init(&table);
    move_class_table_load(&table, &moveinfo.document);
    const MoveClass& tank = table.classes[0];
    CHECK(tank.name == 1 && std::strcmp(table.names[0], "TANKSH2") == 0);
    CHECK(tank.footprint_x == 2 && tank.max_water_depth == 22 && tank.min_water_depth == -10000);
    CHECK(tank.max_slope == 40 && tank.bad_slope == 20);
    CHECK(tank.max_water_slope == 255 && tank.bad_water_slope == 127);
    CHECK(table.classes[1].name == 0 && table.classes[1].bad_slope == 255);
    const MoveClass& boat = table.classes[3];
    // bad_slope comes from the unclamped maxslope, then everything clamps to maxwaterslope.
    CHECK(boat.max_slope == 100 && boat.bad_slope == 100);
    CHECK(boat.max_water_slope == 100 && boat.bad_water_slope == 100);
    CHECK(boat.min_water_depth == 15);
    const MoveClass& steep = table.classes[4];
    CHECK(steep.name == 5 && table.names[4][0] == '\0'); // present with an empty name
    CHECK(steep.max_slope == 10 && steep.bad_slope == 10);
    CHECK(move_class_find(&table, "boat") == 3);
    CHECK(move_class_find(&table, "") == 4);
    CHECK(move_class_find(&table, "missing") == -1);
}

void unit_def_assign_keeps_high_ability_bits() {
    UnitDef source{};
    UnitDef target{};
    oa::base::text::copy_terminated(source.unit_name, "ARMCOM");
    set_abilities(source, 0xff800001u);
    set_abilities(target, 0x00800000u);
    unit_def_assign(&target, &source);
    CHECK(std::strcmp(target.unit_name, "ARMCOM") == 0);
    CHECK(abilities_of(target) == 0x00800001u);
}

std::vector<UnitDef> named_records(const std::vector<std::string>& names) {
    std::vector<UnitDef> records(names.size());
    for (std::size_t i = 0; i < names.size(); ++i) {
        std::memset(&records[i], 0, sizeof records[i]);
        oa::base::text::copy_padded(
            records[i].unit_name, names[i].c_str(), sizeof records[i].unit_name - 1
        );
        records[i].sort_bias = static_cast<int16_t>(i);
        set_flags(records[i], unit_def_flag_available);
    }
    return records;
}

void unit_defs_sort_orders_by_name_nocase() {
    std::vector<std::string> names;
    for (int i = 0; i < 60; ++i) {
        char name[16];
        std::snprintf(
            name, sizeof name, "%c%c%02d", 'A' + (i * 7) % 26, (i & 1) ? 'x' : 'X', (i * 37) % 50
        );
        names.push_back(name);
    }
    auto records = named_records(names);
    unit_defs_sort(records.data(), records.data() + records.size(), unit_def_name_less);
    bool ordered = true;
    std::vector<bool> seen(records.size());
    for (std::size_t i = 0; i < records.size(); ++i) {
        if (i && formats::tdf::compare_nocase(records[i - 1].unit_name, records[i].unit_name) > 0)
            ordered = false;
        seen[static_cast<std::size_t>(records[i].sort_bias)] = true;
    }
    CHECK(ordered);
    CHECK(std::find(seen.begin(), seen.end(), false) == seen.end());
}

void unit_defs_sort_ties_follow_insertion_order() {
    // Short runs use the guarded insertion sort: equal keys keep input order.
    auto records = named_records({"b", "A", "a", "B", "a"});
    unit_defs_sort(records.data(), records.data() + records.size(), unit_def_name_less);
    CHECK(records[0].sort_bias == 1 && records[1].sort_bias == 2 && records[2].sort_bias == 4);
    CHECK(records[3].sort_bias == 0 && records[4].sort_bias == 3);
}

void unit_defs_finalize_catalog_compacts_and_numbers() {
    auto records = named_records({"", "CORAK", "armpw", "HIDDEN", "ARMCOM"});
    set_flags(records[3], 0);
    const uint32_t count = unit_defs_finalize_catalog(records.data(), 5);
    CHECK(count == 4);
    CHECK(std::strcmp(records[1].unit_name, "ARMCOM") == 0 && records[1].type_id == 1);
    CHECK(std::strcmp(records[2].unit_name, "armpw") == 0 && records[2].type_id == 2);
    CHECK(std::strcmp(records[3].unit_name, "CORAK") == 0 && records[3].type_id == 3);
    CHECK(unit_defs_type_id(records.data(), count, "corak") == 3);
    CHECK(unit_defs_type_id(records.data(), count, "hidden") == 0);
    CHECK(unit_defs_find(records.data(), count, "zzz") == nullptr);
}

void categories_register_and_resolve() {
    auto records = named_records({"", "ARMCOM", "ARMPW"});
    unit_defs_finalize_catalog(records.data(), 3);
    CategoryRegistry registry;
    category_registry_init(&registry);
    CHECK(register_unit_categories(&registry, &records[1], "  ARM  COMMANDER\tlevel1 "));
    CHECK(register_unit_categories(&registry, &records[2], "arm KBOT"));
    const CategoryMask* arm = category_registry_find(&registry, "ARM");
    CHECK(arm != nullptr && category_mask_contains(arm, 1) && category_mask_contains(arm, 2));
    const CategoryMask* all = category_registry_find(&registry, "all");
    CHECK(all != nullptr && category_mask_contains(all, 1) && category_mask_contains(all, 2));
    CategoryMaskStorage target_words;
    CategoryMask target = category_mask_over(target_words, registry.words_per_mask * 32);
    CHECK(resolve_type_or_category(&target, &registry, records.data(), 3, "armpw"));
    CHECK(category_mask_contains(&target, 2) && !category_mask_contains(&target, 1));
    CategoryMaskStorage kbot_words;
    CategoryMask kbots = category_mask_over(kbot_words, registry.words_per_mask * 32);
    CHECK(!resolve_type_or_category(&kbots, &registry, records.data(), 3, "KBOT"));
    CHECK(category_mask_contains(&kbots, 2));
    CategoryMaskStorage unknown_words;
    CategoryMask unknown = category_mask_over(unknown_words, registry.words_per_mask * 32);
    CHECK(!resolve_type_or_category(&unknown, &registry, records.data(), 3, "NOSUCH"));
    CHECK(category_registry_find(&registry, "nosuch") != nullptr); // created empty
    category_registry_clear(&registry);
}

oa_ref32 fake_model(void* context, const char*) {
    return ++*static_cast<oa_ref32*>(context);
}

int16_t fake_sound(void*, const char* name) {
    return static_cast<int16_t>(std::strlen(name));
}

void weapon_load_converts_units_and_flags() {
    oa_ref32 models = 100;
    WeaponResolver resolver{&models, fake_model, nullptr, fake_sound};
    WeaponLoadOptions options{&resolver, nullptr, false, false};
    Doc weapons(
        "[LASER]{ID=5; name=Test Laser; weaponvelocity=300; reloadtime=1.5; range=400;"
        " turnrate=16384; weaponacceleration=90; lineofsight=1; turret=1; noautorange=3;"
        " beamweapon=1; model=shell; soundstart=pew; burstrate=0.2;"
        " [DAMAGE]{default=100; ARMCOM=5; corcom=7;}}"
        "[NOID]{name=x;}"
        "[SHARE]{ID=6; model=SHELL; startvelocity=1;}"
    );
    WeaponTable table;
    weapon_table_init(&table);
    for (uint32_t i = 0; i < formats::tdf::child_count(weapons.document.root); ++i)
        weapon_load(&table, formats::tdf::child_at(weapons.document.root, i), &options);
    const WeaponDef& laser = table.defs[5];
    CHECK(std::strcmp(laser.key, "LASER") == 0 && std::strcmp(laser.name, "Test Laser") == 0);
    CHECK(weapon_id(&laser) == 5 && weapon_id(&table.defs[200]) == 200);
    CHECK(laser.weapon_velocity == 655360);
    CHECK(laser.weapon_acceleration == 6553);
    CHECK(laser.reload_time == 45 && laser.burst_rate == 6);
    CHECK(laser.turn_rate == 546);
    CHECK(laser.range == 400 && table.defs[6].range == weapon_default_range);
    CHECK(laser.min_barrel_angle == static_cast<float>(-11.25 * weapon_degrees_to_radians));
    CHECK(
        flags_of(laser) == (weapon_flag_line_of_sight | weapon_flag_turret |
                            weapon_flag_no_auto_range | weapon_flag_beam_weapon)
    );
    CHECK(laser.sound_start == 3 && laser.sound_hit == weapon_no_sound);
    CHECK(laser.damage_default == 100);
    CHECK(weapon_damage_for(&table, &laser, "armcom") == 5);
    CHECK(weapon_damage_for(&table, &laser, "CORCOM") == 7);
    CHECK(weapon_damage_for(&table, &laser, "other") == 100);
    CHECK(table.rejected_ids == 1);
    CHECK(table.past_limit == 0 && table.highest_past_limit == -1);
    // Slot 6 reuses slot 5's model and owns no name of its own.
    CHECK(laser.model == 101 && table.defs[6].model == 101);
    CHECK(
        std::strcmp(weapon_model_name(&laser), "shell") == 0 &&
        weapon_model_name(&table.defs[6])[0] == '\0'
    );
    CHECK(table.defs[6].start_velocity == 2184);
    weapon_table_free(&table);
}

void weapon_same_id_keeps_damage_overrides_and_bit31() {
    Doc weapons("[A]{ID=9; [DAMAGE]{default=1; x=2;}} [B]{ID=9; [DAMAGE]{default=3; y=4;}}");
    WeaponTable table;
    weapon_table_init(&table);
    uint32_t high = 0x80000000u;
    std::memcpy(
        reinterpret_cast<unsigned char*>(&table.defs[9]) + offsetof(WeaponDef, flags), &high, 4
    );
    WeaponLoadOptions options{};
    weapon_load(&table, formats::tdf::child_at(weapons.document.root, 0), &options);
    weapon_load(&table, formats::tdf::child_at(weapons.document.root, 1), &options);
    CHECK(std::strcmp(table.defs[9].key, "B") == 0);
    CHECK(weapon_damage_for(&table, &table.defs[9], "x") == 2);
    CHECK(weapon_damage_for(&table, &table.defs[9], "y") == 4);
    CHECK(flags_of(table.defs[9]) == 0x80000000u);
    weapon_table_free(&table);
}

oa_ref32 fake_animation(void*, const char* gaf, const char*) {
    return gaf[0] == 'l' ? 2u : 1u;
}

void weapon_lava_world_uses_lava_explosion_keys() {
    Doc weapons(
        "[A]{ID=1; waterexplosiongaf=water; waterexplosionart=w; lavaexplosiongaf=lava; "
        "lavaexplosionart=l;}"
    );
    WeaponResolver resolver{nullptr, nullptr, fake_animation, nullptr};
    WeaponTable table;
    weapon_table_init(&table);
    WeaponLoadOptions water{&resolver, nullptr, false, false};
    weapon_load(&table, formats::tdf::child_at(weapons.document.root, 0), &water);
    CHECK(table.defs[1].water_explosion_art == 1 && table.defs[1].explosion_art == 0);
    WeaponLoadOptions lava{&resolver, nullptr, true, false};
    weapon_load(&table, formats::tdf::child_at(weapons.document.root, 0), &lava);
    CHECK(table.defs[1].water_explosion_art == 2);
    weapon_table_free(&table);
}

// A key that is present takes its text's value even with no number in it,
// as the game reads it; only a missing key takes the default.
void weapon_present_keys_without_numbers_read_zero() {
    const char text[] = "[FAR]{ID=1;} [EMPTY]{ID=2; range=; reloadtime=;}"
                        " [WORD]{ID=3; range=far; reloadtime=x; minbarrelangle=;}";
    WeaponTable table;
    weapon_table_init(&table);
    CHECK(load_weapon_text(&table, text, sizeof text - 1, nullptr));
    CHECK(table.defs[1].range == weapon_default_range);
    CHECK(table.defs[2].range == 0 && table.defs[3].range == 0);
    CHECK(table.defs[2].reload_time == 0 && table.defs[3].reload_time == 0);
    CHECK(table.defs[3].min_barrel_angle == 0.0F);
    CHECK(
        table.defs[1].min_barrel_angle ==
        static_cast<float>(weapon_default_min_barrel_angle * weapon_degrees_to_radians)
    );
    weapon_table_free(&table);
}

// The asset names a section gives are kept with the table whatever the
// resolver made of them; a lava world keeps the lava pair in the water names.
void weapon_table_keeps_asset_names() {
    const char text[] = "[A]{ID=4; model=shell; explosiongaf=fx; explosionart=explode3;"
                        " waterexplosiongaf=fx; waterexplosionart=h2o; lavaexplosiongaf=lfx;"
                        " lavaexplosionart=lava; soundstart=pew; soundhit=boom; soundwater=;}"
                        " [B]{ID=5; model=SHELL;}";
    WeaponTable table;
    weapon_table_init(&table);
    CHECK(load_weapon_text(&table, text, sizeof text - 1, nullptr));
    const WeaponAssetNames& a = table.assets[4];
    CHECK(std::strcmp(a.model, "shell") == 0 && std::strcmp(a.explosion_art, "explode3") == 0);
    CHECK(std::strcmp(a.water_explosion_gaf, "fx") == 0);
    CHECK(std::strcmp(a.water_explosion_art, "h2o") == 0);
    CHECK(std::strcmp(a.sound_start, "pew") == 0 && std::strcmp(a.sound_hit, "boom") == 0);
    CHECK(a.sound_water[0] == '\0' && table.defs[4].sound_water == weapon_no_sound);
    // Slot 5 shares slot 4's model but keeps the name it gave.
    CHECK(std::strcmp(table.assets[5].model, "SHELL") == 0);
    CHECK(weapon_model_name(&table.defs[5])[0] == '\0');
    WeaponLoadOptions lava{nullptr, nullptr, true, false};
    CHECK(load_weapon_text(&table, text, sizeof text - 1, &lava));
    CHECK(std::strcmp(table.assets[4].water_explosion_gaf, "lfx") == 0);
    CHECK(std::strcmp(table.assets[4].water_explosion_art, "lava") == 0);
    CHECK(!load_weapon_text(&table, "[C]{name=no id;}", 16, nullptr));
    weapon_table_free(&table);
}

void weapon_files_skip_loose_when_archive_only() {
    test::MemoryFiles memory;
    memory.files = {
        {"weapons\\loose.tdf", "[L]{ID=1;}", false}, {"weapons\\packed.tdf", "[P]{ID=2;}", true}
    };
    const Files files = memory.view();
    WeaponTable table;
    WeaponLoadOptions options{nullptr, nullptr, false, true};
    CHECK(load_weapon_defs(&files, &table, &options) == 1);
    CHECK(table.defs[1].key[0] == '\0' && std::strcmp(table.defs[2].key, "P") == 0);
    options.archive_only = false;
    CHECK(load_weapon_defs(&files, &table, &options) == 2);
    CHECK(std::strcmp(table.defs[1].key, "L") == 0);
    weapon_table_free(&table);
}

// A raised weapon-types limit keeps IDs up to the highest accepted one and
// leaves an ID at the limit out. The default limit keeps 256 slots.
void weapon_table_grows_to_the_highest_accepted_id() {
    oa_ref32 models = 100;
    WeaponResolver resolver{&models, fake_model, nullptr, nullptr};
    const char text[] = "[NEAR]{ID=5; model=shell;}"
                        "[WIDE]{ID=300; model=SHELL;}"
                        "[FAR]{ID=1000; [DAMAGE]{default=8; ARMCOM=3;}}"
                        "[PAST]{ID=2000;}";
    WeaponLoadOptions wide{&resolver, nullptr, false, false, nullptr, 1024};
    WeaponTable table;
    weapon_table_init(&table);
    CHECK(!load_weapon_text(&table, text, sizeof text - 1, &wide));
    CHECK(table.defs.size() == 1001);
    CHECK(
        table.damage.size() == 1001 && table.assets.size() == 1001 && table.rule_data.size() == 1001
    );
    CHECK(std::strcmp(table.defs[300].key, "WIDE") == 0);
    CHECK(std::strcmp(table.defs[1000].key, "FAR") == 0);
    CHECK(oa_weapon_def_id(&table.defs[300]) == 300);
    CHECK(table.defs[299].key[0] == '\0' && oa_weapon_def_id(&table.defs[299]) == 299);
    CHECK(table.past_limit == 1 && table.highest_past_limit == 2000 && table.rejected_ids == 1);
    CHECK(table.defs[5].model != 0 && table.defs[300].model == table.defs[5].model);
    CHECK(weapon_model_name(&table.defs[300])[0] == '\0');
    CHECK(table.defs[1000].damage_overrides == 1001);
    CHECK(weapon_damage_for(&table, &table.defs[1000], "ARMCOM") == 3);
    CHECK(find_weapon_by_name(table.defs, "FAR") == 1000);

    CategoryRegistry categories;
    category_registry_init(&categories);
    UnitDefBlocks blocks{};
    unit_def_blocks_init(&blocks);
    test::MemoryFiles memory;
    memory.files = {{"units\\wide.fbi", "[UNITINFO]{UnitName=WIDE; Weapon1=far; BMcode=1;}"}};
    const Files files = memory.view();
    const UnitDefSources sources{"", nullptr, table.defs, nullptr, &categories, &blocks, nullptr};
    UnitDef unit{};
    CHECK(load_unit_def(&files, "units\\wide.fbi", unit, sources));
    CHECK(unit.weapon1 == 1001);
    category_registry_clear(&categories);
    for (uint32_t block = 0; block < blocks.count; ++block)
        std::free(blocks.blocks[block].bytes);
    std::free(blocks.blocks);

    WeaponTable limited;
    weapon_table_init(&limited);
    CHECK(!load_weapon_text(&limited, text, sizeof text - 1, nullptr));
    CHECK(limited.defs.size() == OA_WEAPON_DEF_COUNT);
    CHECK(
        limited.rejected_ids == 3 && limited.past_limit == 3 && limited.highest_past_limit == 2000
    );
    CHECK(std::strcmp(limited.defs[5].key, "NEAR") == 0);
    CHECK(limited.defs[6].key[0] == '\0');
    weapon_table_free(&limited);

    const char base_text[] = "[A]{ID=5;}[B]{ID=200;}";
    WeaponLoadOptions raised{nullptr, nullptr, false, false, nullptr, 1024};
    WeaponTable base;
    weapon_table_init(&base);
    CHECK(load_weapon_text(&base, base_text, sizeof base_text - 1, &raised));
    CHECK(base.defs.size() == OA_WEAPON_DEF_COUNT);
    CHECK(base.past_limit == 0 && base.rejected_ids == 0 && base.highest_past_limit == -1);
    weapon_table_free(&base);
    weapon_table_free(&table);
}

std::string sidedata_fixture(bool with_reload3) {
    const char* rects[] = {"LOGO",           "ENERGYBAR",     "ENERGYNUM",     "METALBAR",
                           "METALNUM",       "TOTALUNITS",    "TOTALTIME",     "ENERGY0",
                           "METAL0",         "ENERGYMAX",     "METALMAX",      "ENERGYPRODUCED",
                           "ENERGYCONSUMED", "METALPRODUCED", "METALCONSUMED", "LOGO2",
                           "UNITNAME",       "DAMAGEBAR",     "UNITMETALMAKE", "UNITMETALUSE",
                           "UNITENERGYMAKE", "UNITENERGYUSE", "MISSIONTEXT",   "UNITNAME2",
                           "DAMAGEBAR2",     "NAME",          "DESCRIPTION",   "RELOAD1",
                           "RELOAD2",        "RELOAD3"};
    std::string text =
        "[SIDE0]{name=ARM; nameprefix=ARMX; commander=ARMCOM; energycolor=208; metalcolor=224;";
    int n = 0;
    for (const char* rect : rects) {
        if (!with_reload3 && std::strcmp(rect, "RELOAD3") == 0)
            continue;
        text += "[" + std::string(rect) + "]{x1=" + std::to_string(n) + "; y1=1; x2=2; y2=3;}";
        ++n;
    }
    return text + "}";
}

void sides_load_hud_layout() {
    const std::string text = sidedata_fixture(true) + "[SIDE2]{name=GHOST;}";
    Doc sidedata(text.c_str());
    SideTable table;
    CHECK(side_table_load(&sidedata.document, &table, nullptr));
    CHECK(table.count == 1);
    const Side& arm = table.sides[0];
    CHECK(std::strcmp(arm.name, "ARM") == 0 && std::strcmp(arm.name_prefix, "ARM") == 0);
    CHECK(std::strcmp(arm.commander, "ARMCOM") == 0);
    CHECK(arm.energy_color == 208 && arm.metal_color == 224);
    CHECK(arm.rect_logo.x1 == 0 && arm.rect_energy_bar.x1 == 1 && arm.rect_description.x1 == 26);
    CHECK(arm.rect_reload3.x1 == 29 && arm.rect_reload3.y2 == 3);
    CHECK(side_index(&table.sides[0]) == 0 && side_index(&table.sides[1]) == 1);
}

void sides_name_their_panel_gaf() {
    // SIDE0 names its panel GAF, SIDE1 names none and SIDE2 one longer than
    // the name's 29 characters.
    const std::string side = sidedata_fixture(true);
    const std::string body = side.substr(std::strlen("[SIDE0]{"));
    const std::string text = "[SIDE0]{intgaf=ARMINT; " + body + "[SIDE1]{" + body +
                             "[SIDE2]{intgaf=PANELS456789012345678901234567890; " + body;
    Doc sidedata(text.c_str());
    SideTable table;
    CHECK(side_table_load(&sidedata.document, &table, nullptr));
    CHECK(table.count == 3);
    CHECK(std::strcmp(table.panel_gaf[0], "ARMINT") == 0);
    CHECK(table.panel_gaf[1][0] == '\0');
    CHECK(std::strcmp(table.panel_gaf[2], "PANELS45678901234567890123456") == 0);
}

void sides_missing_rect_is_reported() {
    const std::string text = sidedata_fixture(false);
    Doc sidedata(text.c_str());
    SideTable table;
    CHECK(!side_table_load(&sidedata.document, &table, nullptr));
    CHECK(
        std::strcmp(
            table.error, "Section [RELOAD3] is missing from GAMEDATA/SIDEDATA.TDF for the ARM side"
        ) == 0
    );
}

void sound_categories_collect_numbered_choices() {
    Doc sound(
        "[ARM_KBOT]{select=kbselect; select1=kbsel1; select1text=Ready; select3=gap; ok1=kbok;"
        " canceldestruct=cancel;}[EMPTY]{}"
    );
    SoundCategoryTable table;
    CHECK(sound_category_table_load(&sound.document, &table));
    CHECK(table.count == 2);
    const SoundCategory& kbot = table.categories[0];
    CHECK(std::strcmp(kbot.name, "ARM_KBOT") == 0);
    CHECK(kbot.events[0].count == 0);
    const SoundChoices& select = kbot.events[1];
    CHECK(select.count == 2); // stops at the missing select2
    CHECK(
        std::strcmp(select.sounds[1], "kbsel1") == 0 && std::strcmp(select.texts[1], "Ready") == 0
    );
    CHECK(select.texts[0][0] == '\0');
    CHECK(kbot.events[5].count == 1 && std::strcmp(kbot.events[5].sounds[0], "kbok") == 0);
    CHECK(kbot.events[23].count == 1);
    CHECK(sound_category_find(&table, "empty") == 1);
    sound_category_table_free(&table);
}

struct CachedSounds {
    int clears = 0;
    std::vector<std::string> entries;
};

SoundCache cache_of(CachedSounds& cached) {
    return SoundCache{
        &cached,
        [](void* context) {
            auto& sounds = *static_cast<CachedSounds*>(context);
            ++sounds.clears;
            sounds.entries.clear();
        },
        [](void* context, const char* name, const char* sound) {
            static_cast<CachedSounds*>(context)->entries.push_back(std::string(name) + "=" + sound);
        },
    };
}

void all_sounds_cache_top_level_sections_with_a_sound() {
    CachedSounds cached;
    const SoundCache cache = cache_of(cached);
    test::MemoryFiles memory;
    memory.files = {
        {"gamedata\\allsound.tdf",
         "[BIGBUTTON]{sound=butmain1;}[silent]{priority=1;}[wrapper]{[nested]{sound=inner;}}"
         "[ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789]{sound=long;}[Empty]{sound=;}"},
        {"gamedata\\sound.tdf", "[ARM_KBOT]{select=kbselect;}"},
    };
    const Files files = memory.view();
    SoundCategoryTable categories{};
    load_all_sounds(&files, nullptr, cache, &categories);
    CHECK(cached.clears == 1);
    CHECK(cached.entries.size() == 3);
    CHECK(cached.entries.size() == 3 && cached.entries[0] == "BIGBUTTON=butmain1");
    CHECK(
        cached.entries.size() == 3 && cached.entries[1] == "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345=long"
    );
    CHECK(cached.entries.size() == 3 && cached.entries[2] == "Empty=");
    CHECK(categories.count == 1);
    sound_category_table_free(&categories);

    // Without allsound.tdf the cache is only cleared; the categories still load.
    test::MemoryFiles sound_only;
    sound_only.files = {{"gamedata\\sound.tdf", "[A]{}[B]{}"}};
    const Files only = sound_only.view();
    load_all_sounds(&only, nullptr, cache, &categories);
    CHECK(cached.clears == 2 && cached.entries.empty());
    CHECK(categories.count == 2);
    sound_category_table_free(&categories);
}

void locale_translate_and_reverse() {
    Doc translate(
        "[Single Player]{french=Solo; "
        "german=Einzelspieler;}[Options]{german=Optionen;}[Exit]{french=;}"
    );
    LocaleTable table;
    locale_table_init(&table);
    CHECK(locale_table_load(&table, &translate.document, "French"));
    CHECK(table.count == 1);
    CHECK(std::strcmp(locale_translate(&table, "Single Player"), "Solo") == 0);
    CHECK(
        std::strcmp(locale_translate(&table, "single player"), "single player") == 0
    ); // exact case
    CHECK(std::strcmp(locale_translate(&table, "Options"), "Options") == 0);
    CHECK(std::strcmp(locale_find_source(&table, "SOLO"), "Single Player") == 0);
    CHECK(locale_table_load(&table, &translate.document, "FRENCH") && table.count == 1);
    CHECK(locale_table_load(&table, &translate.document, "german") && table.count == 2);
    locale_table_free(&table);

    Doc mission("[M]{missionname=War; frenchmissionname=Guerre;}");
    const formats::tdf::Block* block = formats::tdf::child_at(mission.document.root, 0);
    char out[32];
    CHECK(get_localized_string(block, "french", "missionname", out, sizeof out, nullptr));
    CHECK(std::strcmp(out, "Guerre") == 0);
    CHECK(get_localized_string(block, "german", "missionname", out, sizeof out, nullptr));
    CHECK(std::strcmp(out, "War") == 0);
}

/// What a unit text sink receives: the unit, the language's place and the texts.
struct ReceivedUnitText {
    std::string unit;
    uint32_t language{};
    std::string name;
    std::string description;
    bool has_name{};
    bool has_description{};
};

/// Collects what read_unit_texts hands its sink.
///
/// @param context the vector of ReceivedUnitText
/// @param unit_name the unit's name
/// @param language the language's place in the sink's list
/// @param name the name, or null
/// @param description the description, or null
void receive_unit_text(
    void* context,
    const char* unit_name,
    uint32_t language,
    const char* name,
    const char* description
) {
    auto& received = *static_cast<std::vector<ReceivedUnitText>*>(context);
    received.push_back(
        {unit_name,
         language,
         name != nullptr ? name : "",
         description != nullptr ? description : "",
         name != nullptr,
         description != nullptr}
    );
}

void unit_texts_read_language_keys() {
    // 3.1c's Arm Commander, its French description keyed with a lower-case
    // d as the game data has it, and no Italian text; a name longer than
    // the field is cut as the field cuts it.
    Doc fbi(
        "[UNITINFO]{UnitName=ARMCOM;Name=Commander;Description=Commander;"
        "GermanName=Commander;GermanDescription=Kommandant;FrenchName=Commandeur;"
        "Frenchdescription=Commandant;SpanishName=Comandante;"
        "PiglatinName=Ommandercay Ommandercay Ommandercay Ommandercay;}"
    );
    const formats::tdf::Block* block = formats::tdf::child_at(fbi.document.root, 0);
    const std::array<const char*, 5> words{"German", "french", "Italian", "SPANISH", "piglatin"};
    std::vector<ReceivedUnitText> received;
    const UnitTextSink sink{
        &received, words.data(), static_cast<uint32_t>(words.size()), receive_unit_text
    };
    read_unit_texts(block, "ARMCOM", &sink);
    CHECK(received.size() == 4);
    if (received.size() == 4) {
        CHECK(received[0].unit == "ARMCOM" && received[0].language == 0);
        CHECK(received[0].name == "Commander" && received[0].description == "Kommandant");
        CHECK(received[1].language == 1 && received[1].name == "Commandeur");
        CHECK(received[1].description == "Commandant");
        CHECK(received[2].language == 3 && received[2].name == "Comandante");
        CHECK(received[2].has_name && !received[2].has_description);
        CHECK(received[3].language == 4);
        CHECK(received[3].name.size() == unit_text_name_bytes - 1);
    }
    // No sink, no block or no languages read nothing.
    read_unit_texts(block, "ARMCOM", nullptr);
    read_unit_texts(nullptr, "ARMCOM", &sink);
    const UnitTextSink none{&received, nullptr, 0, receive_unit_text};
    read_unit_texts(block, "ARMCOM", &none);
    CHECK(received.size() == 4);
    // A word too long for a key reads nothing.
    const std::string long_word(300, 'x');
    const std::array<const char*, 1> long_words{long_word.c_str()};
    const UnitTextSink long_sink{&received, long_words.data(), 1, receive_unit_text};
    read_unit_texts(block, "ARMCOM", &long_sink);
    CHECK(received.size() == 4);

    // The unit loader hands its sink the same texts and keeps Name and
    // Description in the record, whatever the language.
    test::MemoryFiles files;
    files.files = {
        {"units\\armcom.fbi",
         "[UNITINFO]{UnitName=ARMCOM;Name=Commander;Description=Commander;"
         "FrenchName=Commandeur;Frenchdescription=Commandant;}",
         true}
    };
    const Files view = files.view();
    WeaponTdfSet weapons;
    weapon_tdf_set_init(&weapons);
    std::vector<ReceivedUnitText> loaded;
    const UnitTextSink loader_sink{&loaded, words.data(), 2, receive_unit_text};
    UnitHeaderSources header_sources{"", &weapons, 3, 1, false, false};
    header_sources.texts = &loader_sink;
    UnitDef header{};
    bool refused = false;
    CHECK(load_unit_header(&view, "units\\armcom.fbi", header, header_sources, &refused));
    CHECK(std::strcmp(header.name, "Commander") == 0);
    CHECK(loaded.size() == 1 && loaded[0].name == "Commandeur");
    weapon_tdf_set_free(&weapons);
}

void paths_follow_the_game_quirks() {
    char path[path_capacity];
    oa::base::text::copy_terminated(path, "maps\\v1.0\\thing");
    remove_extension(path); // cuts at a dot in the directory
    CHECK(std::strcmp(path, "maps\\v1") == 0);
    format_with_extension("maps\\v1.0\\thing", path, sizeof path, "ota");
    CHECK(std::strcmp(path, "maps\\v1.0\\thing.ota") == 0);
    format_with_extension("units\\armcom.fbi", path, sizeof path, "cob");
    CHECK(std::strcmp(path, "units\\armcom.cob") == 0);
    oa::base::text::copy_terminated(path, "a\\b\\c.txt");
    truncate_to_directory(path);
    CHECK(std::strcmp(path, "a\\b\\") == 0);
    oa::base::text::copy_terminated(path, "plain");
    truncate_to_directory(path);
    CHECK(std::strcmp(path, "plain") == 0);

    test::MemoryFiles memory;
    memory.files = {{"gamedata-mod\\sidedata.tdf", "x=1;"}};
    const Files files = memory.view();
    build_variant_path(&files, path, sizeof path, "gamedata", "sidedata", "tdf", "mod");
    CHECK(std::strcmp(path, "gamedata-mod\\sidedata.tdf") == 0);
    build_variant_path(&files, path, sizeof path, "gamedata", "moveinfo.tdf", "tdf", "mod");
    CHECK(std::strcmp(path, "gamedata\\moveinfo.tdf") == 0);
    build_variant_path(&files, path, sizeof path, "gamedata", "", "tdf", nullptr);
    CHECK(path[0] == '\0');
}

// The named-GAF read joins the GUI's GAF directory ("anims\\")
// and the name, replaces any extension with GAF, and fails for a file of size 0.
void named_gaf_reads_from_the_gui_directory() {
    test::MemoryFiles memory;
    memory.files = {
        {"anims\\Greenbrief.GAF", "GAFDATA"}, {"anims\\Empty.gaf", ""}, {"Bare.gaf", "X"}
    };
    const Files files = memory.view();
    uint8_t* data = nullptr;
    uint32_t size = 0;
    CHECK(read_named_gaf(&files, gui_gaf_directory, "Greenbrief", &data, &size));
    CHECK(size == 7 && std::memcmp(data, "GAFDATA", 7) == 0);
    files.release(files.context, data);
    CHECK(read_named_gaf(&files, gui_gaf_directory, "Greenbrief.pcx", &data, &size) && size == 7);
    files.release(files.context, data);
    CHECK(!read_named_gaf(&files, gui_gaf_directory, "Empty", &data, &size));
    CHECK(!read_named_gaf(&files, gui_gaf_directory, "Absent", &data, &size));
    CHECK(read_named_gaf(&files, "", "Bare", &data, &size) && size == 1);
    files.release(files.context, data);
}

void load_tdf_file_refuses_empty_and_marks_archive() {
    test::MemoryFiles memory;
    memory.files = {{"a.tdf", "", false}, {"b.tdf", "[X]{}", true}};
    const Files files = memory.view();
    formats::tdf::Document document;
    formats::tdf::document_init(&document);
    CHECK(!load_tdf_file(&files, "a.tdf", &document, nullptr));
    CHECK(!load_tdf_file(&files, "missing.tdf", &document, nullptr));
    CHECK(load_tdf_file(&files, "B.TDF", &document, nullptr));
    CHECK(document.from_archive);
    formats::tdf::document_free(&document);
}

struct LoaderFixture {
    MoveClassTable moves{};
    std::vector<WeaponDef> weapon_defs = std::vector<WeaponDef>(OA_WEAPON_DEF_COUNT);
    SoundCategory sound_records[2]{};
    SoundCategoryTable sounds{sound_records, 2};
    CategoryRegistry categories{};
    UnitDefBlocks blocks{};
    std::vector<std::string> corpses;
    UnitDefLoadHost host{this, [](void* context, const char* name) -> int16_t {
                             auto* self = static_cast<LoaderFixture*>(context);
                             self->corpses.emplace_back(name);
                             return 42;
                         }};

    LoaderFixture() {
        move_class_table_init(&moves);
        Doc moveinfo(
            "[CLASS2]{name=KBOTSH2; footprintx=3; footprintz=1; maxwaterdepth=22; maxslope=15;}"
        );
        move_class_table_load(&moves, &moveinfo.document);
        std::memset(weapon_defs.data(), 0, sizeof(WeaponDef) * weapon_defs.size());
        oa::base::text::copy_terminated(weapon_defs[0].key, "NOWEAPON");
        oa::base::text::copy_terminated(weapon_defs[7].key, "LASER");
        oa::base::text::copy_terminated(weapon_defs[9].key, "BLAST");
        oa::base::text::copy_terminated(sound_records[0].name, "ARM_SOLAR");
        oa::base::text::copy_terminated(sound_records[1].name, "ARM_KBOT");
        category_registry_init(&categories);
        unit_def_blocks_init(&blocks);
    }

    ~LoaderFixture() {
        category_registry_clear(&categories);
        for (uint32_t block = 0; block < blocks.count; ++block)
            std::free(blocks.blocks[block].bytes);
        std::free(blocks.blocks);
    }

    LoaderFixture(const LoaderFixture&) = delete;
    LoaderFixture& operator=(const LoaderFixture&) = delete;

    UnitDefSources sources(const char* language) {
        return {language, &moves, weapon_defs, &sounds, &categories, &blocks, &host};
    }
};

void unit_def_loader_reads_kbot_keys() {
    test::MemoryFiles memory;
    memory.files = {
        {"units\\armtest.fbi",
         "[UNITINFO]{UnitName=ARMTEST; Name=Tester; FrenchName=Testeur; Description=Tests;"
         " MovementClass=kbotsh2; FootprintX=9; Weapon1=laser; Weapon3=nosuch; ExplodeAs=BLAST;"
         " SoundCategory=arm_kbot; MaxDamage=500; BuildCostMetal=12; BuildCostEnergy=90;"
         " MaxVelocity=1.2; BrakeRate=0.3; Acceleration=0.15; TurnRate=700; Builder=1;"
         " StandingMoveOrder=0; CloakCost=200; CanReclamate=1; canmove=1; firestandorders=1;"
         " SelfDestructCountdown=9; wpri_badTargetCategory=VTOL; Category=ARM KBOT;"
         " Corpse=armtest_dead; DefaultMissionType=Standby; waterline=300; BMcode=1;}"}
    };
    const Files files = memory.view();
    LoaderFixture fixture;
    UnitDef unit{};
    unit.type_id = 3;
    unit.flags = OA_UNIT_DEF_FLAG_AVAILABLE;
    CHECK(load_unit_def(&files, "units\\ARMTEST.FBI", unit, fixture.sources("French")));
    CHECK(
        std::strcmp(unit.unit_name, "ARMTEST") == 0 && std::strcmp(unit.object_name, "ARMTEST") == 0
    );
    CHECK(std::strcmp(unit.name, "Testeur") == 0 && std::strcmp(unit.description, "Tests") == 0);
    CHECK(
        unit.default_mission_type ==
        static_cast<int8_t>(data::mission_types::index_for_name("Standby"))
    );
    CHECK(unit.default_mission_type != 0);
    // Missing bad-target keys name "none"; the primary names VTOL.
    CHECK(unit.primary_bad_target_category == category_registry_ref(&fixture.categories, "VTOL"));
    CHECK(unit.secondary_bad_target_category == category_registry_ref(&fixture.categories, "none"));
    CHECK(unit.special_bad_target_category == unit.secondary_bad_target_category);
    CHECK(unit.no_chase_category == unit.secondary_bad_target_category);
    CHECK(
        unit.build_cost_metal == 12.0F && unit.build_cost_energy == 90.0F && unit.max_damage == 500
    );
    // 16.16 values truncate after the x65536 scale; move rates default to twice the velocity.
    CHECK(unit.max_velocity == 78643 && unit.brake_rate == 19660 && unit.acceleration == 9830);
    CHECK(unit.bank_scale == 0x10000 && unit.damage_modifier == 0x10000 && unit.pitch_scale == 0);
    CHECK(unit.move_rate1 == 157286 && unit.move_rate2 == 157286);
    CHECK(unit.turn_rate == 700 && unit.water_line == 44); // 300 narrowed to a byte
    // Standing fire order defaults to 2; the move order reads 0.
    CHECK(
        (unit.flags & (OA_UNIT_DEF_FLAG_MOVE_ORDER_MASK | OA_UNIT_DEF_FLAG_FIRE_ORDER_MASK)) == 0x8
    );
    CHECK(
        (unit.flags & OA_UNIT_DEF_FLAG_AVAILABLE) != 0 &&
        (unit.flags & OA_UNIT_DEF_FLAG_BUILDER) != 0
    );
    CHECK((unit.flags & OA_UNIT_DEF_FLAG_HAS_WEAPONS) != 0);
    // canreclamate also grants repair; cloakcost grants cloak and the 80 minimum distance.
    CHECK(
        unit.abilities ==
        (OA_UNIT_DEF_ABILITY_FIRE_STAND_ORDERS | OA_UNIT_DEF_ABILITY_CAN_MOVE |
         OA_UNIT_DEF_ABILITY_CAN_REPAIR | OA_UNIT_DEF_ABILITY_CAN_RECLAMATE |
         OA_UNIT_DEF_ABILITY_CAN_CLOAK | (1u << OA_UNIT_DEF_ABILITY_SELF_DESTRUCT_SHIFT))
    );
    CHECK(
        unit.cloak_cost == 200.0F && unit.cloak_cost_moving == 200.0F &&
        unit.min_cloak_distance == 80
    );
    CHECK(unit.sound_category == 1);
    CHECK(unit.corpse == 42 && fixture.corpses.size() == 1 && fixture.corpses[0] == "armtest_dead");
    // The movement class supplies footprint, depth and slope; FootprintX=9 is ignored.
    CHECK(unit.move_class == 3 && unit.footprint_x == 3 && unit.footprint_z == 1);
    CHECK(unit.max_water_depth == 22 && unit.min_water_depth == -10000);
    CHECK(
        unit.max_slope == 15 && unit.max_water_slope == 255 && unit.slope_speed_step == 78643 / 16
    );
    // Unknown and missing weapons point at record 0.
    CHECK(unit.weapon1 == 8 && unit.weapon2 == 1 && unit.weapon3 == 1);
    CHECK(unit.explode_as == 10 && unit.self_destruct_as == 1);
    CHECK(unit.yard_map == 0 && unit.bm_code == 1); // only buildings compile a yard map
    CHECK(
        unit.bounds_min_x == -0x180000 && unit.bounds_max_x == 0x180000 && unit.size_x == 0x300000
    );
    CHECK(unit.bounds_min_z == -0x80000 && unit.bounds_max_z == 0x80000 && unit.size_z == 0x100000);
    CHECK(unit.size_radius == 0x400000 / 3);
    const CategoryMask* kbot = category_registry_find(&fixture.categories, "kbot");
    const CategoryMask* all = category_registry_find(&fixture.categories, "ALL");
    CHECK(
        kbot != nullptr && category_mask_contains(kbot, 3) && all != nullptr &&
        category_mask_contains(all, 3)
    );
    CHECK(!load_unit_def(&files, "units\\missing.fbi", unit, fixture.sources("")));
}

void unit_def_loader_reads_building_keys() {
    test::MemoryFiles memory;
    memory.files = {
        {"units\\armpad.fbi",
         "[UNITINFO]{UnitName=ARMPAD; ObjectName=pad3; BMcode=0; FootprintX=3; FootprintZ=2;"
         " MaxSlope=40; MaxWaterSlope=20; YardMap=oO c; Weapon2=NOWEAPON; SoundCategory=17;"
         " MinCloakDistance=5; CloakCost=4; DamageModifier=0.33333; StandingFireOrder=7;}"},
        {"units\\armtiny.fbi",
         "[UNITINFO]{UnitName=ARMTINY; BMcode=0; FootprintX=2; FootprintZ=2;"
         " YardMap=yY; MovementClass=NOSUCH;}"}
    };
    const Files files = memory.view();
    LoaderFixture fixture;
    UnitDef pad{};
    pad.model_height = 0x50000;
    CHECK(load_unit_def(&files, "units\\armpad.fbi", pad, fixture.sources("")));
    CHECK(std::strcmp(pad.object_name, "pad3") == 0 && pad.name[0] == '\0');
    CHECK(pad.move_class == 0 && pad.footprint_x == 3 && pad.footprint_z == 2);
    // The unit's own slopes clamp to the water slope, as a movement class would.
    CHECK(pad.max_slope == 20 && pad.max_water_slope == 20);
    CHECK(pad.max_water_depth == 10000 && pad.min_water_depth == -10000);
    CHECK(pad.damage_modifier == 21845);
    CHECK(
        (pad.flags & OA_UNIT_DEF_FLAG_FIRE_ORDER_MASK) == (3u << OA_UNIT_DEF_FLAG_FIRE_ORDER_SHIFT)
    );
    CHECK(
        (pad.abilities & OA_UNIT_DEF_ABILITY_SELF_DESTRUCT_MASK) ==
        (5u << OA_UNIT_DEF_ABILITY_SELF_DESTRUCT_SHIFT)
    );
    CHECK(pad.min_cloak_distance == 5 && pad.cloak_cost_moving == 4.0F);
    CHECK(
        (pad.flags & OA_UNIT_DEF_FLAG_HAS_WEAPONS) == 0 && pad.weapon2 == 1
    );                               // NOWEAPON is record 0
    CHECK(pad.sound_category == 17); // no category matches
    CHECK(pad.corpse == -1 && fixture.corpses.empty());
    CHECK(pad.size_y == 0x50000 && pad.size_radius == 0x500000 / 3);
    const uint8_t* cells = unit_def_block(&fixture.blocks, pad.yard_map);
    CHECK(cells != nullptr && unit_def_block_size(&fixture.blocks, pad.yard_map) == 6);
    // Spaces separate; the last cell repeats once the text runs out.
    if (cells != nullptr)
        CHECK(
            cells[0] == 0x2f && cells[1] == 0x2b && cells[2] == 0x2d && cells[3] == 0x2d &&
            cells[5] == 0x2d
        );
    UnitDef tiny{};
    CHECK(load_unit_def(&files, "units\\armtiny.fbi", tiny, fixture.sources("")));
    CHECK(tiny.move_class == 0 && tiny.footprint_x == 2 && tiny.max_slope == 255);
    // With neither StandingMoveOrder nor StandingFireOrder a unit roams (2) and
    // fires at will (2).
    CHECK((tiny.flags & OA_UNIT_DEF_FLAG_MOVE_ORDER_MASK) == 2u);
    CHECK(
        (tiny.flags & OA_UNIT_DEF_FLAG_FIRE_ORDER_MASK) == (2u << OA_UNIT_DEF_FLAG_FIRE_ORDER_SHIFT)
    );
    const uint8_t* tiny_cells = unit_def_block(&fixture.blocks, tiny.yard_map);
    CHECK(
        tiny_cells != nullptr && tiny_cells[0] == 0x29 && tiny_cells[1] == 0x31 &&
        tiny_cells[3] == 0x31
    );
}

void catalog_build_lists_and_download_menu() {
    test::MemoryFiles memory;
    memory.files = {
        {"gamedata\\sidedata.tdf",
         "[CANBUILD]{[ARMCOM]{canbuild1=ARMPW; canbuild2=NOPE; canbuild3=armalab;}}"},
        {"download\\zeus.tdf",
         "[MENUENTRY1]{UNITMENU=ARMALAB; MENU=3; BUTTON=2; UNITNAME=ARMZEUS;}"
         "[MENUENTRY2]{UNITMENU=armcom; MENU=2; BUTTON=0; UNITNAME=ARMZEUS;}"},
        {"download\\pw.tdf", "[MENUENTRY1]{UNITMENU=ARMPW; MENU=9; UNITNAME=ARMCOM;}"},
    };
    const Files files = memory.view();
    UnitDefTables tables;
    unit_def_tables_init(&tables);
    CHECK(unit_def_tables_allocate(&tables, 5));
    const char* names[] = {"None", "ARMALAB", "ARMCOM", "ARMPW", "ARMZEUS"};
    for (uint16_t index = 0; index < 5; ++index) {
        oa::base::text::copy_terminated(tables.records[index].unit_name, names[index]);
        tables.records[index].type_id = index;
    }
    tables.records[1].flags = OA_UNIT_DEF_FLAG_BUILDER;
    tables.records[1].gui_page_count = 1;
    tables.records[2].flags = OA_UNIT_DEF_FLAG_BUILDER | OA_UNIT_DEF_FLAG_DOWNLOADABLE;
    tables.records[2].gui_page_count = 4;
    CHECK(load_build_lists(&files, nullptr, &tables));
    const uint16_t* commander = unit_def_build_ids(&tables, tables.records[2]);
    CHECK(
        tables.records[2].build_id_count == 2 && commander != nullptr && commander[0] == 3 &&
        commander[1] == 1
    );
    CHECK(tables.records[1].build_ids != 0 && tables.records[1].build_id_count == 0);
    CHECK(tables.records[3].build_ids == 0);
    CHECK(load_download_menu(&files, nullptr, &tables));
    CHECK(tables.downloads.count == 2);
    const DownloadMenuGroup* zeus = &tables.downloads.groups[0];
    if (std::strcmp(zeus->entries[0].unit_name, "ARMZEUS") != 0)
        zeus = &tables.downloads.groups[1];
    CHECK(zeus->count == 2 && zeus->entries[0].builder_index == 1 && zeus->entries[0].menu == 3);
    CHECK(zeus->entries[0].button == 2 && zeus->entries[1].builder_index == 2);
    // Pages only grow; a download MENU names the builder's last page.
    CHECK(tables.records[1].gui_page_count == 3 && tables.records[2].gui_page_count == 4);
    CHECK(tables.records[3].gui_page_count == 9);
    CHECK((tables.records[4].flags & OA_UNIT_DEF_FLAG_DOWNLOADABLE) != 0);
    CHECK((tables.records[2].flags & OA_UNIT_DEF_FLAG_DOWNLOADABLE) != 0);
    const uint16_t* lab = unit_def_build_ids(&tables, tables.records[1]);
    CHECK(tables.records[1].build_id_count == 1 && lab != nullptr && lab[0] == 4);
    CHECK(tables.records[2].build_id_count == 3 && commander[2] == 4);
    CHECK(tables.records[3].build_ids == 0); // not a builder: its download entry adds nothing
    unit_def_tables_free(&tables);
    CHECK(tables.records == nullptr && tables.count == 0 && tables.blocks.count == 0);
}

/// Loads one builder's list of CANBUILD entries, and two download entries for
/// it, under a set of build-list limits.
///
/// @param lists the build-list limits
/// @param[out] count the builder's build_id_count after the CANBUILD pass
/// @param[out] with_downloads its build_id_count after the download menus
/// @param[out] in_order whether the kept entries are the first ones, in order
/// @param listed the CANBUILD entries, at most 1100
void load_long_build_list(
    const data::limits::BuildLists& lists,
    uint32_t& count,
    uint32_t& with_downloads,
    bool& in_order,
    uint32_t listed = 1100
) {
    std::string sidedata = "[CANBUILD]{[BUILDER]{";
    char entry[48];
    for (uint32_t index = 0; index < listed; ++index) {
        std::snprintf(entry, sizeof entry, "canbuild%u=U%04u;", index + 1, index);
        sidedata += entry;
    }
    sidedata += "}}";
    test::MemoryFiles memory;
    memory.files = {
        {"gamedata\\sidedata.tdf", sidedata},
        {"download\\more.tdf",
         "[MENUENTRY1]{UNITMENU=BUILDER; MENU=2; UNITNAME=U0001;}"
         "[MENUENTRY2]{UNITMENU=BUILDER; MENU=2; UNITNAME=U0002;}"},
    };
    const Files files = memory.view();
    UnitDefTables tables;
    unit_def_tables_init(&tables);
    // Every unit the entries and the download menus name exists, however few
    // entries the list names.
    constexpr uint32_t named = 1100;
    CHECK(unit_def_tables_allocate(&tables, named + 2));
    oa::base::text::copy_terminated(tables.records[1].unit_name, "BUILDER");
    tables.records[1].type_id = 1;
    tables.records[1].flags = OA_UNIT_DEF_FLAG_BUILDER;
    for (uint32_t index = 0; index < named; ++index) {
        UnitDef& unit = tables.records[index + 2];
        std::snprintf(unit.unit_name, sizeof unit.unit_name, "U%04u", index);
        unit.type_id = static_cast<uint16_t>(index + 2);
    }
    CHECK(load_build_lists(&files, nullptr, &tables, lists));
    count = tables.records[1].build_id_count;
    const uint16_t* list = unit_def_build_ids(&tables, tables.records[1]);
    in_order = list != nullptr;
    for (uint32_t index = 0; in_order && index < count; ++index)
        in_order = list[index] == index + 2;
    CHECK(load_download_menu(&files, nullptr, &tables, lists));
    with_downloads = tables.records[1].build_id_count;
    unit_def_tables_free(&tables);
}

void catalog_build_lists_follow_limits() {
    uint32_t count = 0;
    uint32_t with_downloads = 0;
    bool in_order = false;
    // 3.1c keeps 30 CANBUILD entries; the download menus add one more.
    load_long_build_list({}, count, with_downloads, in_order);
    CHECK(count == 30 && in_order && with_downloads == 31);
    // A raised copy keeps 36. The download menus add entries only while a list
    // holds fewer than 31, so they add nothing to it.
    load_long_build_list(
        {36, data::limits::BuildListOverflow::truncate}, count, with_downloads, in_order
    );
    CHECK(count == 36 && in_order && with_downloads == 36);
    // To a shorter list under the same copy they add entries until it holds 31.
    load_long_build_list(
        {36, data::limits::BuildListOverflow::truncate}, count, with_downloads, in_order, 30
    );
    CHECK(count == 30 && in_order && with_downloads == 31);
    // The largest copy a profile may name.
    load_long_build_list(
        {data::limits::highest_build_list_copy, data::limits::BuildListOverflow::truncate},
        count,
        with_downloads,
        in_order
    );
    CHECK(count == data::limits::highest_build_list_copy && in_order);
    CHECK(with_downloads == data::limits::highest_build_list_copy);
    // A list that keeps every entry stops at the largest copy too, and takes
    // one download entry more.
    load_long_build_list(
        {30, data::limits::BuildListOverflow::dynamic}, count, with_downloads, in_order
    );
    CHECK(count == data::limits::highest_build_list_copy && in_order);
    CHECK(with_downloads == data::limits::highest_build_list_copy + 1);
}

void gamedata_tables_follow_tableinfo() {
    test::MemoryFiles memory;
    memory.files = {
        {"gamedata\\los.tdf",
         "[TABLEINFO]{numtables=3;}[TABLE1]{numlines=2; line1=1,0,1;}"
         "[TABLE3]{numlines=-4;}[TABLE4]{numlines=1;}"}
    };
    const Files files = memory.view();

    struct Seen {
        int16_t tables = -1;
        std::vector<std::string> calls;
    } seen;

    const GamedataTableSink sink{
        &seen,
        [](void* context, int16_t count) {
            static_cast<Seen*>(context)->tables = count;
            return true;
        },
        [](void* context, int16_t index, const formats::tdf::Block* section, int16_t lines) {
            char line[16];
            formats::tdf::get_string(section, "line1", line, sizeof line, "-");
            static_cast<Seen*>(context)->calls.push_back(
                std::to_string(index) + ":" + std::to_string(lines) + ":" + line
            );
            return true;
        },
    };
    CHECK(load_gamedata_tables(&files, nullptr, sink));
    // TABLE2 is absent and TABLE4 is past numtables; a negative numlines reads as none.
    CHECK(seen.tables == 3);
    CHECK((seen.calls == std::vector<std::string>{"0:2:1,0,1", "2:0:-"}));
    test::MemoryFiles empty;
    const Files none = empty.view();
    CHECK(!load_gamedata_tables(&none, nullptr, sink));
}

void palette_file_prefers_pal_then_pcx() {
    test::MemoryFiles memory;
    std::string pal(0x400, '\0');
    pal[4] = 7;
    memory.files = {
        {"palettes\\palette.pal", pal}, {"palettes\\short.pal", "abc"}, {"palettes\\empty.pal", ""}
    };
    const Files files = memory.view();
    std::vector<std::string> images;
    PaletteImageReader reader{
        &images, [](void* context, const char* path, uint8_t* palette) {
            static_cast<std::vector<std::string>*>(context)->emplace_back(path);
            palette[0] = 9;
            return true;
        }
    };
    uint8_t palette[palette_file_bytes]{};
    CHECK(
        load_palette_file(&files, "PALETTE", reader, palette) && palette[4] == 7 && images.empty()
    );
    CHECK(!load_palette_file(&files, "short", reader, palette) && images.empty());
    // A missing or empty .PAL falls back to the image of the same name.
    CHECK(load_palette_file(&files, "empty", reader, palette) && palette[0] == 9);
    CHECK(load_palette_file(&files, "guipal", reader, palette));
    CHECK((images == std::vector<std::string>{"palettes\\empty.PCX", "palettes\\guipal.PCX"}));
    CHECK(!load_palette_file(&files, "guipal", {nullptr, nullptr}, palette));
}

/// Returns the Copyright value the unit check accepts, with a year filled in.
///
/// @param year the four year digits
/// @return unit_copyright with its zero year replaced by `year`
std::string unit_copyright_of_year(const char* year) {
    std::string text(unit_copyright);
    text.replace(text.find("0000"), std::strlen(year), year);
    return text;
}

void unit_header_hashes_and_checks_each_file() {
    const std::string weapons_text = "[LASER]{id=7; range=5;}[BLAST]{id=9;}";
    const std::string armtest =
        "[UNITINFO]{UnitName=ARMTEST; Name=Tester; Side=ARM; ai_weight=w; ai_limit=l; "
        "BuildCostEnergy=90;"
        " norestrict=1; wacky=1; Weapon1=LASER; ExplodeAs=blast; Weapon3=NOSUCH; Version=3.1;"
        " Copyright=" +
        unit_copyright_of_year("1998") + ";}";
    test::MemoryFiles memory;
    memory.files = {
        {"weapons\\a.tdf", weapons_text, true},
        {"weapons\\loose.tdf", "[LASER]{id=1;}", false},
        {"units\\armtest.fbi", armtest, true},
        {"units\\newer.fbi",
         "[UNITINFO]{UnitName=NEWER; Version=3.2; Copyright=" + unit_copyright_of_year("1997") +
             ";}",
         true},
        {"units\\pirate.fbi",
         "[UNITINFO]{UnitName=PIRATE; Version=1; Copyright=Copyright 1997 Nobody.;}",
         true},
        {"units\\loose.fbi",
         "[UNITINFO]{UnitName=LOOSE; ObjectName=shell; Copyright=" +
             unit_copyright_of_year("1997") + ";}",
         false},
    };
    const Files files = memory.view();
    WeaponTdfSet weapons;
    weapon_tdf_set_init(&weapons);
    CHECK(load_weapon_tdf_set(&files, nullptr, true, &weapons));
    CHECK(weapons.capacity == 2 && weapons.count == 1); // the loose file is skipped
    Doc reference(weapons_text.c_str());
    const uint32_t laser = formats::tdf::find_child(reference.document.root, "LASER")->body_hash;
    const uint32_t blast = formats::tdf::find_child(reference.document.root, "BLAST")->body_hash;
    CHECK(laser != 0 && laser != blast);
    CHECK(weapon_tdf_hash(&weapons, "laser") == laser && weapon_tdf_hash(&weapons, "") == 0);
    CHECK(weapon_tdf_hash(&weapons, "NOSUCH") == 0 && weapon_tdf_hash(&weapons, nullptr) == 0);

    const UnitHeaderSources sources{"", &weapons, 3, 1, true, false};
    UnitDef unit{};
    bool refused = false;
    CHECK(load_unit_header(&files, "units\\ARMTEST.FBI", unit, sources, &refused));
    CHECK(
        unit.fbi_hash ==
        formats::tdf::buffer_hash(
            reinterpret_cast<const uint8_t*>(armtest.data()), static_cast<int32_t>(armtest.size())
        )
    );
    CHECK(std::strcmp(unit.unit_name, "ARMTEST") == 0 && std::strcmp(unit.name, "Tester") == 0);
    CHECK(std::strcmp(unit.side, "ARM") == 0 && std::strcmp(unit.object_name, "ARMTEST") == 0);
    CHECK(std::strcmp(unit.ai_weight, "w") == 0 && std::strcmp(unit.ai_limit, "l") == 0);
    CHECK(unit.build_cost_energy == 90.0F && unit.player_limit == -1);
    CHECK(unit.abilities == (OA_UNIT_DEF_ABILITY_NO_RESTRICT | OA_UNIT_DEF_ABILITY_WACKY));
    CHECK(unit.weapon_checksum == (laser ^ blast));
    // Version 3.1 is the running build; the copyright year is not compared.
    CHECK((unit.flags & OA_UNIT_DEF_FLAG_AVAILABLE) != 0 && !refused);

    UnitDef newer{};
    CHECK(load_unit_header(&files, "units\\newer.fbi", newer, sources, &refused));
    CHECK((newer.flags & OA_UNIT_DEF_FLAG_AVAILABLE) == 0 && !refused);
    UnitDef pirate{};
    CHECK(load_unit_header(&files, "units\\pirate.fbi", pirate, sources, &refused));
    CHECK((pirate.flags & OA_UNIT_DEF_FLAG_AVAILABLE) == 0 && refused);
    refused = false;
    UnitDef loose{};
    CHECK(load_unit_header(&files, "units\\loose.fbi", loose, sources, &refused));
    CHECK((loose.flags & OA_UNIT_DEF_FLAG_AVAILABLE) == 0 && refused);
    CHECK(std::strcmp(loose.object_name, "shell") == 0);
    refused = false;
    const UnitHeaderSources any_source{"", &weapons, 3, 1, false, false};
    CHECK(load_unit_header(&files, "units\\loose.fbi", loose, any_source, &refused));
    CHECK((loose.flags & OA_UNIT_DEF_FLAG_AVAILABLE) != 0 && !refused);
    CHECK(!load_unit_header(&files, "units\\missing.fbi", loose, any_source, &refused));
    weapon_tdf_set_free(&weapons);
    CHECK(weapons.documents == nullptr && weapons.count == 0);
}

void revision_gpf_version_is_checked() {
    test::MemoryFiles current;
    current.files = {{"gamedata\\version.tdf", "[Version]{GPFVersion=V3.0;}"}};
    test::MemoryFiles older;
    older.files = {{"gamedata\\version.tdf", "[Version]{GPFVersion=v1.0;}"}};
    test::MemoryFiles keyless;
    keyless.files = {{"gamedata\\version.tdf", "[Version]{}"}};
    test::MemoryFiles none;
    const Files current_files = current.view();
    const Files older_files = older.view();
    const Files keyless_files = keyless.view();
    const Files no_files = none.view();
    CHECK(!revision_gpf_mismatch(&current_files, nullptr));
    CHECK(revision_gpf_mismatch(&older_files, nullptr));
    CHECK(revision_gpf_mismatch(&keyless_files, nullptr));
    CHECK(!revision_gpf_mismatch(&no_files, nullptr));
}

void update_unit_def_reloads_a_catalog_type() {
    test::MemoryFiles memory;
    memory.files = {
        {"units\\armtest.fbi", "[UNITINFO]{UnitName=ARMTEST; MaxDamage=900; BMcode=1;}"}
    };
    const Files files = memory.view();
    LoaderFixture fixture;
    UnitDefTables tables;
    unit_def_tables_init(&tables);
    CHECK(unit_def_tables_allocate(&tables, 3));
    oa::base::text::copy_terminated(tables.records[1].unit_name, "ARMTEST");
    tables.records[1].type_id = 1;
    tables.records[1].flags = OA_UNIT_DEF_FLAG_AVAILABLE;
    tables.records[1].max_damage = 5;
    oa::base::text::copy_terminated(tables.records[2].unit_name, "ARMGONE");
    tables.records[2].type_id = 2;
    std::vector<std::string> scripts;
    const UnitScriptLoader loader{&scripts, [](void* context, uint16_t type, const char* path) {
                                      static_cast<std::vector<std::string>*>(context)->push_back(
                                          std::to_string(type) + ":" + path
                                      );
                                  }};
    const UnitDefSources sources = fixture.sources("");
    CHECK(update_unit_def(&files, &tables, 1, sources, loader));
    CHECK(tables.records[1].max_damage == 900);
    CHECK((scripts == std::vector<std::string>{"1:scripts\\ARMTEST.COB"}));
    // Out of the catalog, reserved, or beyond the table: nothing reloads.
    CHECK(!update_unit_def(&files, &tables, 2, sources, loader));
    CHECK(!update_unit_def(&files, &tables, 0, sources, loader));
    CHECK(!update_unit_def(&files, &tables, 3, sources, loader));
    CHECK(scripts.size() == 1);
    unit_def_tables_free(&tables);
}

// The header pass runs while the table is empty or a full load marked it
// stale, and the mark is cleared either way.
void unit_headers_reload_when_empty_or_stale() {
    auto game = std::make_unique<Game>();
    int32_t loads = 0;
    const UnitHeaderLoader loader{&loads, [](void* context, Game* loaded) {
                                      ++*static_cast<int32_t*>(context);
                                      loaded->unit_def_count = 7;
                                  }};
    reload_unit_headers(game.get(), loader);
    CHECK(loads == 1 && game->unit_def_count == 7 && game->unit_defs_stale == 0);
    reload_unit_headers(game.get(), loader);
    CHECK(loads == 1);
    game->unit_defs_stale = 1;
    reload_unit_headers(game.get(), loader);
    CHECK(loads == 2 && game->unit_defs_stale == 0);
}

} // namespace

int main() {
    move_class_bad_slope_defaults_and_clamps();
    unit_def_assign_keeps_high_ability_bits();
    unit_defs_sort_orders_by_name_nocase();
    unit_defs_sort_ties_follow_insertion_order();
    unit_defs_finalize_catalog_compacts_and_numbers();
    categories_register_and_resolve();
    weapon_load_converts_units_and_flags();
    weapon_same_id_keeps_damage_overrides_and_bit31();
    weapon_lava_world_uses_lava_explosion_keys();
    weapon_present_keys_without_numbers_read_zero();
    weapon_table_keeps_asset_names();
    weapon_files_skip_loose_when_archive_only();
    weapon_table_grows_to_the_highest_accepted_id();
    sides_load_hud_layout();
    sides_name_their_panel_gaf();
    sides_missing_rect_is_reported();
    sound_categories_collect_numbered_choices();
    all_sounds_cache_top_level_sections_with_a_sound();
    locale_translate_and_reverse();
    unit_texts_read_language_keys();
    paths_follow_the_game_quirks();
    named_gaf_reads_from_the_gui_directory();
    load_tdf_file_refuses_empty_and_marks_archive();
    unit_def_loader_reads_kbot_keys();
    unit_def_loader_reads_building_keys();
    catalog_build_lists_and_download_menu();
    catalog_build_lists_follow_limits();
    gamedata_tables_follow_tableinfo();
    palette_file_prefers_pal_then_pcx();
    unit_header_hashes_and_checks_each_file();
    revision_gpf_version_is_checked();
    update_unit_def_reloads_a_catalog_type();
    unit_headers_reload_when_empty_or_stale();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("defs: all checks passed");
    return 0;
}
