// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Characterisation of the definition loaders that read a whole file through
// the file boundary: movement classes, sound categories, side data and the
// translation table, each with its variant-directory choice and its
// missing, empty and malformed files, plus category mask references.

#include "oa/data/defs/categories.hpp"
#include "oa/data/defs/locale.hpp"
#include "oa/data/defs/move_classes.hpp"
#include "oa/data/defs/sides.hpp"
#include "oa/data/defs/sound_categories.hpp"

#include "test_files.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>

namespace {

using namespace oa::data::defs;
using oa::oa_ref32;
using oa::data::defs::test::MemoryFiles;
namespace tdf = oa::formats::tdf;

int failures = 0;

/// Records a failed check.
///
/// @param line source line of the check
/// @param what text of the failed condition
void report_failure(int line, const char* what) {
    std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, line, what);
    ++failures;
}

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            report_failure(__LINE__, #condition);                                                  \
        }                                                                                          \
    } while (false)

constexpr const char* variant = "v1";
constexpr const char* missing_variant = "v2";

// SIDEDATA.TDF sub-sections every side needs, in the order the loader asks for them.
constexpr const char* side_rect_sections[] = {
    "LOGO",           "ENERGYBAR",      "ENERGYNUM",      "METALBAR",      "METALNUM",
    "TOTALUNITS",     "TOTALTIME",      "ENERGY0",        "METAL0",        "ENERGYMAX",
    "METALMAX",       "ENERGYPRODUCED", "ENERGYCONSUMED", "METALPRODUCED", "METALCONSUMED",
    "LOGO2",          "UNITNAME",       "DAMAGEBAR",      "UNITMETALMAKE", "UNITMETALUSE",
    "UNITENERGYMAKE", "UNITENERGYUSE",  "MISSIONTEXT",    "UNITNAME2",     "DAMAGEBAR2",
    "NAME",           "DESCRIPTION",    "RELOAD1",        "RELOAD2",       "RELOAD3",
};

/// Writes a SIDEn section with every rectangle, rectangle k spanning (k, 2k)-(k + 10, 2k + 5).
///
/// @param index side number
/// @param name side name
/// @param skipped rectangle section left out; null for none
/// @return the section text
std::string side_section(int32_t index, const char* name, const char* skipped) {
    std::string text = "[SIDE" + std::to_string(index) + "]{name=" + name + "; nameprefix=" + name +
                       "; commander=" + name + "COM; font=" + name +
                       "FONT; energycolor=208; metalcolor=224;";
    int32_t k = 0;
    for (const char* section : side_rect_sections) {
        if (skipped == nullptr || std::strcmp(section, skipped) != 0) {
            text += std::string("[") + section + "]{x1=" + std::to_string(k) +
                    "; y1=" + std::to_string(2 * k) + "; x2=" + std::to_string(k + 10) +
                    "; y2=" + std::to_string(2 * k + 5) + ";}";
        }
        ++k;
    }
    return text + "}";
}

/// Returns the length of a font name, as the test resolver's handle.
///
/// @param context unused
/// @param name font name
/// @return the name's length
oa_ref32 font_handle(void* context, const char* name) {
    (void)context;
    return static_cast<oa_ref32>(std::strlen(name));
}

/// Checks MOVEINFO loading: the variant file first, then the base file.
void test_load_move_classes() {
    MemoryFiles files;
    files.files.push_back(
        {"gamedata/MOVEINFO.TDF", "[CLASS0]{name=TANKSH2; footprintx=2;}[CLASS3]{name=BOAT4;}"}
    );
    files.files.push_back({"gamedata-v1/moveinfo.tdf", "[CLASS1]{name=KBOTSH2; maxslope=40;}"});
    const Files view = files.view();
    MoveClassTable table;
    tdf::ParseError error{};

    CHECK(load_move_classes(&view, &table, nullptr, &error));
    CHECK(move_class_find(&table, "tanksh2") == 0 && move_class_find(&table, "BOAT4") == 3);
    CHECK(
        table.classes[0].name == 1 && table.classes[0].footprint_x == 2 &&
        table.classes[1].name == 0
    );

    // The variant file replaces the base file entirely.
    CHECK(load_move_classes(&view, &table, variant, &error));
    CHECK(move_class_find(&table, "tanksh2") == -1 && move_class_find(&table, "KBOTSH2") == 1);
    CHECK(table.classes[1].max_slope == 40 && table.classes[1].bad_slope == 20);

    // A variant without its own file falls back to the base file.
    CHECK(load_move_classes(&view, &table, missing_variant, &error));
    CHECK(move_class_find(&table, "BOAT4") == 3);

    // Missing, empty and malformed files fail and leave every slot reset.
    MemoryFiles empty;
    empty.files.push_back({"gamedata/moveinfo.tdf", ""});
    const Files empty_view = empty.view();
    CHECK(!load_move_classes(&empty_view, &table, nullptr, &error));
    CHECK(table.classes[3].name == 0 && table.classes[3].max_slope == 0xFF);
    MemoryFiles broken;
    broken.files.push_back({"gamedata/moveinfo.tdf", "[CLASS0]{name=A; footprintx=2"});
    const Files broken_view = broken.view();
    error = tdf::ParseError{};
    CHECK(!load_move_classes(&broken_view, &table, nullptr, &error));
    CHECK(error.status != tdf::ParseStatus::ok);
    MemoryFiles none;
    const Files none_view = none.view();
    CHECK(!load_move_classes(&none_view, &table, nullptr, nullptr));
    CHECK(!load_move_classes(nullptr, &table, nullptr, nullptr));
    CHECK(table.classes[0].name == 0);
}

/// Checks SOUND.TDF loading into one category per section.
void test_load_sound_categories() {
    MemoryFiles files;
    files.files.push_back(
        {"gamedata/sound.tdf",
         "[ARM_TANK]{select=tnkslct; select1=tnkslct2; selecttext=Ready; ok=tnkok;}"
         "[CORE_KBOT]{select2=never; underattack=hit;}"}
    );
    files.files.push_back({"gamedata-v1/sound.tdf", "[ONLY]{cant=nope;}"});
    const Files view = files.view();
    SoundCategoryTable table{};

    CHECK(load_sound_categories(&view, &table, nullptr));
    CHECK(table.count == 2 && sound_category_find(&table, "arm_tank") == 0);
    if (table.count == 2) {
        const SoundCategory& tank = table.categories[0];
        // Event i of sound_event_keys fills slot i + 1: select is slot 1, ok slot 5.
        CHECK(tank.events[1].count == 2 && std::strcmp(tank.events[1].sounds[1], "tnkslct2") == 0);
        CHECK(
            std::strcmp(tank.events[1].texts[0], "Ready") == 0 && tank.events[1].texts[1][0] == '\0'
        );
        CHECK(tank.events[5].count == 1 && tank.events[0].count == 0);
        // KEY2 without KEY1 is never reached; KEY itself is optional.
        const SoundCategory& kbot = table.categories[1];
        CHECK(kbot.events[1].count == 0 && kbot.events[2].count == 1);
    }
    sound_category_table_free(&table);

    CHECK(load_sound_categories(&view, &table, variant));
    CHECK(table.count == 1 && sound_category_find(&table, "ONLY") == 0);
    sound_category_table_free(&table);

    MemoryFiles none;
    const Files none_view = none.view();
    CHECK(!load_sound_categories(&none_view, &table, nullptr));
    CHECK(table.count == 0 && table.categories == nullptr);
}

/// Checks SIDEDATA.TDF loading, its required rectangles and its font resolver.
void test_load_side_data() {
    MemoryFiles files;
    files.files.push_back(
        {"gamedata/sidedata.tdf",
         side_section(0, "ARM", nullptr) + side_section(1, "CORE", nullptr)}
    );
    const Files view = files.view();
    const SideFontResolver fonts{nullptr, font_handle};
    SideTable table{};

    CHECK(load_side_data(&view, &table, nullptr, &fonts));
    CHECK(table.count == 2 && std::strcmp(table.sides[1].name, "CORE") == 0);
    CHECK(std::strcmp(table.sides[0].commander, "ARMCOM") == 0 && table.sides[0].font == 7);
    CHECK(table.sides[0].energy_color == 208 && table.sides[0].metal_color == 224);
    // METALBAR is the fourth rectangle read (k = 3), RELOAD3 the last (k = 29).
    CHECK(table.sides[1].rect_metal_bar.x1 == 3 && table.sides[1].rect_metal_bar.y2 == 11);
    CHECK(table.sides[0].rect_reload3.x2 == 39 && table.sides[0].rect_reload3.y1 == 58);
    CHECK(side_index(&table.sides[1]) == 1 && side_index(&table.sides[2]) == 2);

    // A side missing a rectangle stops the load and names it.
    MemoryFiles incomplete;
    incomplete.files.push_back(
        {"gamedata/sidedata.tdf",
         side_section(0, "ARM", nullptr) + side_section(1, "CORE", "METALBAR")}
    );
    const Files incomplete_view = incomplete.view();
    CHECK(!load_side_data(&incomplete_view, &table, nullptr, nullptr));
    CHECK(table.count == 0);
    CHECK(
        std::strcmp(
            table.error,
            "Section [METALBAR] is missing from GAMEDATA/SIDEDATA.TDF for the CORE side"
        ) == 0
    );

    // A missing file still loads the empty document: no sides, no error text.
    MemoryFiles none;
    const Files none_view = none.view();
    CHECK(!load_side_data(&none_view, &table, variant, nullptr));
    CHECK(table.count == 0 && table.error[0] == '\0');
}

/// Checks each side's intgaf and font names, their paths and the files missing from the game data.
void test_side_files() {
    // SIDE0 and SIDE1 name their panels and font, SIDE2 neither, SIDE3 an
    // empty font.
    const auto with_keys = [](std::string section, const std::string& keys) {
        section.insert(section.find('{') + 1, keys);
        return section;
    };
    const auto without_font = [](std::string section) {
        const auto at = section.find("font=");
        section.erase(at, section.find(';', at) + 2 - at);
        return section;
    };
    const std::string sidedata =
        with_keys(side_section(0, "ARM", nullptr), "intgaf=ARMPANEL; ") +
        with_keys(side_section(1, "CORE", nullptr), "intgaf=COREPANEL; ") +
        without_font(side_section(2, "THIRD", nullptr)) +
        with_keys(without_font(side_section(3, "FOURTH", nullptr)), "font=; ");
    MemoryFiles files;
    files.files.push_back({"gamedata/sidedata.tdf", sidedata});
    files.files.push_back({"anims/ARMPANEL.GAF", "GAF"});
    files.files.push_back({"anims/COREPANEL.GAF", "GAF"});
    files.files.push_back({"fonts/ARMFONT.FNT", "FNT"});
    files.files.push_back({"fonts/COREFONT.FNT", "FNT"});
    const Files view = files.view();
    SideTable table{};

    CHECK(load_side_data(&view, &table, nullptr, nullptr));
    CHECK(table.count == 4);
    CHECK(table.has_panel_gaf[0] && table.has_panel_gaf[1] && !table.has_panel_gaf[2]);
    CHECK(table.has_font[0] && table.has_font[1] && !table.has_font[2] && table.has_font[3]);
    CHECK(std::strcmp(table.panel_gaf[1], "COREPANEL") == 0);
    CHECK(std::strcmp(table.font_name[1], "COREFONT") == 0 && table.font_name[3][0] == '\0');
    char path[path_capacity];
    CHECK(side_file_path(&view, table, 0, SideFile::panels, variant, path, sizeof path));
    CHECK(std::strcmp(path, "anims\\ARMPANEL.GAF") == 0);
    CHECK(side_file_path(&view, table, 1, SideFile::font, nullptr, path, sizeof path));
    CHECK(std::strcmp(path, "fonts\\COREFONT.FNT") == 0);
    // A side that names no file has no path; past the last side, neither.
    CHECK(!side_file_path(&view, table, 2, SideFile::font, nullptr, path, sizeof path));
    CHECK(path[0] == '\0');
    CHECK(!side_file_path(&view, table, 4, SideFile::panels, nullptr, path, sizeof path));
    // An empty font name is named, but no file there can be.
    SideMissingFile missing[side_missing_file_capacity];
    CHECK(side_missing_files(&view, table, nullptr, missing) == 1);
    CHECK(missing[0].side == 3 && missing[0].file == SideFile::font);
    CHECK(std::strcmp(missing[0].path, "fonts/.FNT") == 0);

    // The variant directory comes first; a file only it holds is not missing
    // while the variant is played.
    files.files.erase(files.files.begin() + 1);
    files.files.push_back({"anims-v1/ARMPANEL.GAF", "GAF"});
    CHECK(side_file_path(&view, table, 0, SideFile::panels, variant, path, sizeof path));
    CHECK(std::strcmp(path, "anims-v1\\ARMPANEL.GAF") == 0);
    CHECK(side_missing_files(&view, table, variant, missing) == 1);
    // Every side's intgaf comes before any side's font: SIDE1's panels, then
    // SIDE0's font, then SIDE3's.
    files.files.erase(files.files.begin() + 2); // fonts/ARMFONT.FNT
    files.files.erase(files.files.begin() + 1); // anims/COREPANEL.GAF
    CHECK(side_missing_files(&view, table, variant, missing) == 3);
    CHECK(missing[0].side == 1 && missing[0].file == SideFile::panels);
    CHECK(std::strcmp(missing[0].path, "anims/COREPANEL.GAF") == 0);
    CHECK(missing[1].side == 0 && missing[1].file == SideFile::font);
    CHECK(std::strcmp(missing[1].path, "fonts/ARMFONT.FNT") == 0);
    CHECK(missing[2].side == 3 && missing[2].file == SideFile::font);
    // Without the variant, the plain file is missing too and listed by its
    // plain path, with the other panels.
    CHECK(side_missing_files(&view, table, nullptr, missing) == 4);
    CHECK(missing[0].side == 0 && missing[0].file == SideFile::panels);
    CHECK(std::strcmp(missing[0].path, "anims/ARMPANEL.GAF") == 0);
    CHECK(missing[1].side == 1 && missing[1].file == SideFile::panels);
    CHECK(missing[2].side == 0 && missing[2].file == SideFile::font);
    CHECK(missing[3].side == 3);
    // A list too short for them all still counts them.
    CHECK(side_missing_files(&view, table, nullptr, std::span(missing, 1)) == 4);
    CHECK(std::strcmp(missing[0].path, "anims/ARMPANEL.GAF") == 0);
}

/// Checks translation loading and its language switch.
void test_load_locale_table() {
    MemoryFiles files;
    files.files.push_back(
        {"gamedata/lang.tdf",
         "[Single Player]{french=Solo; german=Einzelspieler;}[Options]{french=Options;}"
         "[Quit]{german=Beenden;}"}
    );
    const Files view = files.view();
    LocaleTable table;
    locale_table_init(&table);

    CHECK(load_locale_table(&view, &table, "gamedata\\lang.tdf", "french"));
    CHECK(table.count == 2 && std::strcmp(table.language, "french") == 0);
    CHECK(std::strcmp(locale_translate(&table, "Single Player"), "Solo") == 0);
    CHECK(std::strcmp(locale_translate(&table, "Quit"), "Quit") == 0);

    // The loaded language, in any case, is not read again.
    CHECK(load_locale_table(&view, &table, "gamedata\\missing.tdf", "FRENCH"));
    CHECK(table.count == 2);

    CHECK(load_locale_table(&view, &table, "gamedata\\lang.tdf", "german"));
    CHECK(table.count == 2 && std::strcmp(locale_translate(&table, "Quit"), "Beenden") == 0);

    // A missing file switches the language and leaves the table empty.
    CHECK(!load_locale_table(&view, &table, "gamedata\\missing.tdf", "spanish"));
    CHECK(table.count == 0 && std::strcmp(table.language, "spanish") == 0);

    // A UTF-8 translation longer than its buffer is cut between whole
    // characters: 85 three-byte characters keep 85 of 255 bytes, never 255.
    std::string long_text;
    for (int index = 0; index < 90; ++index)
        long_text += "\xE4\xB8\xAD";
    MemoryFiles chinese;
    chinese.files.push_back({"gamedata/lang.tdf", "[Long]{chinese=" + long_text + ";}"});
    const Files chinese_view = chinese.view();
    CHECK(load_locale_table(&chinese_view, &table, "gamedata\\lang.tdf", "chinese"));
    const std::string cut = locale_translate(&table, "Long");
    CHECK(cut.size() == 85 * 3 && long_text.starts_with(cut));
    locale_table_free(&table);
}

/// Checks that mask references resolve to their masks and to nothing outside the registry.
void test_category_registry_mask() {
    CategoryRegistry registry;
    category_registry_init(&registry);
    CHECK(category_registry_mask(&registry, 1) == nullptr);

    const oa_ref32 vtol = category_registry_ref(&registry, "VTOL");
    const oa_ref32 noweapon = category_registry_ref(&registry, "NOWEAPON");
    const oa_ref32 armor = category_registry_ref(&registry, "ARM");
    // Masks keep creation order whatever the name order.
    CHECK(vtol == 1 && noweapon == 2 && armor == 3);
    CHECK(category_registry_ref(&registry, "vtol") == vtol);

    CategoryMask* mask = category_registry_mask(&registry, noweapon);
    CHECK(mask == &registry.masks[1]);
    category_mask_set(mask, 300);
    const CategoryRegistry& view = registry;
    const CategoryMask* read = category_registry_mask(&view, noweapon);
    CHECK(read == mask && category_mask_contains(read, 300) && !category_mask_contains(read, 301));
    CHECK(!category_mask_contains(category_registry_mask(&view, vtol), 300));

    CHECK(category_registry_mask(&registry, 0) == nullptr);
    CHECK(category_registry_mask(&registry, 4) == nullptr);
    CHECK(category_registry_mask(&view, 0xFFFFFFFFu) == nullptr);
    category_registry_clear(&registry);
    CHECK(category_registry_mask(&registry, 1) == nullptr);
}

/// Registers one unit type id in a category and in "ALL".
///
/// @param[in,out] registry registry to register in
/// @param type_id the unit's type id
/// @return what register_unit_categories returns
bool register_type(CategoryRegistry& registry, uint16_t type_id) {
    oa::UnitDef unit{};
    unit.type_id = type_id;
    return register_unit_categories(&registry, &unit, "WIDE");
}

/// Checks the category mask width: 512 type ids as in 3.1c, ids past it left out of
/// every category, and the widest registry holding every 16-bit type id.
void test_category_mask_width() {
    CategoryRegistry base;
    category_registry_init(&base);
    CHECK(base.words_per_mask == category_mask_words);
    CHECK(register_type(base, 511) && register_type(base, 512) && register_type(base, 600));
    const CategoryMask* wide = category_registry_find(&base, "WIDE");
    const CategoryMask* all = category_registry_find(&base, "ALL");
    CHECK(wide != nullptr && wide->word_count == category_mask_words);
    CHECK(category_mask_contains(wide, 511) && category_mask_contains(all, 511));
    CHECK(!category_mask_contains(wide, 512) && !category_mask_contains(wide, 600));
    CHECK(!category_mask_contains(all, 600));
    // A registry that holds a category keeps its width.
    CHECK(!category_registry_set_mask_types(&base, oa::data::limits::highest_type_bits));
    category_registry_clear(&base);
    CHECK(base.words_per_mask == category_mask_words);

    CategoryRegistry widest;
    category_registry_init(&widest);
    CHECK(!category_registry_set_mask_types(&widest, oa::data::limits::highest_type_bits + 1));
    CHECK(category_registry_set_mask_types(&widest, oa::data::limits::highest_type_bits));
    CHECK(widest.words_per_mask == max_category_mask_words);
    CHECK(register_type(widest, 600) && register_type(widest, 65535));
    // Enough further categories to grow the registry past its first 64, which
    // moves the words every mask points into.
    char name[16];
    for (uint32_t index = 0; index < 100; ++index) {
        std::snprintf(name, sizeof name, "C%u", index);
        CategoryMask* mask = category_registry_find_or_add(&widest, name);
        CHECK(mask != nullptr && mask->word_count == max_category_mask_words);
        if (mask != nullptr)
            category_mask_set(mask, static_cast<uint16_t>(60000 + index));
    }
    wide = category_registry_find(&widest, "WIDE");
    all = category_registry_find(&widest, "ALL");
    CHECK(
        wide != nullptr && category_mask_contains(wide, 600) && category_mask_contains(wide, 65535)
    );
    CHECK(
        all != nullptr && category_mask_contains(all, 65535) && !category_mask_contains(all, 601)
    );
    const CategoryMask* last = category_registry_find(&widest, "C99");
    CHECK(
        last != nullptr && category_mask_contains(last, 60099) &&
        !category_mask_contains(last, 60098)
    );
    // Words outside a registry hold as many type ids as asked, up to the widest.
    CategoryMaskStorage storage;
    CategoryMask target = category_mask_over(storage, oa::data::limits::highest_type_bits);
    category_mask_or(&target, wide);
    CHECK(target.word_count == max_category_mask_words && category_mask_contains(&target, 65535));
    CategoryMask narrow = category_mask_over(storage, oa::data::limits::base_type_bits);
    category_mask_or(&narrow, wide);
    CHECK(narrow.word_count == category_mask_words && !category_mask_contains(&narrow, 600));
    category_registry_clear(&widest);
    CHECK(widest.words_per_mask == max_category_mask_words);
}

/// Builds SIDE0 .. SIDE<count - 1>, each named S<n> so the prefix fits.
///
/// @param count how many sections
/// @param broken_last rectangle left out of the last section; null for none
/// @return the sections
std::string side_sections(int32_t count, const char* broken_last) {
    std::string text;
    for (int32_t index = 0; index < count; ++index) {
        const std::string name = "S" + std::to_string(index);
        const char* skipped = index + 1 == count ? broken_last : nullptr;
        text += side_section(index, name.c_str(), skipped);
    }
    return text;
}

/// Loads `text` through the file boundary at the given side limit.
///
/// @param text SIDEDATA.TDF body
/// @param side_limit sides the load may take
/// @param[out] table table to fill
/// @return what load_side_data returned
bool load_text(const std::string& text, uint32_t side_limit, SideTable& table) {
    MemoryFiles files;
    files.files.push_back({"gamedata/sidedata.tdf", text});
    const Files view = files.view();
    return load_side_data(&view, &table, nullptr, nullptr, side_limit);
}

/// Checks the side limit, the sections left unread past it, and a side past the fifth.
void test_side_limit() {
    const std::string seven = side_sections(7, nullptr);
    MemoryFiles files;
    files.files.push_back({"gamedata/sidedata.tdf", seven});
    const Files view = files.view();
    SideTable table{};
    char line[256];

    CHECK(load_side_data(&view, &table, nullptr, nullptr, 7));
    CHECK(table.count == 7 && table.limit == 7 && table.past_limit == 0);
    CHECK(std::strcmp(table.sides[6].name, "S6") == 0);
    CHECK(std::strcmp(table.sides[6].name_prefix, "S6") == 0);
    CHECK(std::strcmp(table.sides[6].commander, "S6COM") == 0);
    line[0] = 'x';
    CHECK(!side_table_past_limit_line(table, line, sizeof line));
    CHECK(line[0] == '\0');

    CHECK(load_side_data(&view, &table, nullptr, nullptr, 10));
    CHECK(table.count == 7 && table.limit == 10 && table.past_limit == 0);
    line[0] = 'x';
    CHECK(!side_table_past_limit_line(table, line, sizeof line));
    CHECK(line[0] == '\0');

    char path[path_capacity];
    CHECK(side_file_path(&view, table, 6, SideFile::font, nullptr, path, sizeof path));
    CHECK(std::strcmp(path, "fonts\\S6FONT.FNT") == 0);
    SideMissingFile missing[side_missing_file_capacity];
    const uint32_t gaps = side_missing_files(&view, table, nullptr, missing);
    bool saw_side_6 = false;
    const uint32_t listed = gaps < side_missing_file_capacity ? gaps : side_missing_file_capacity;
    for (uint32_t index = 0; index < listed; ++index)
        if (missing[index].side == 6 && missing[index].file == SideFile::font)
            saw_side_6 = true;
    CHECK(saw_side_6);

    CHECK(load_side_data(&view, &table, nullptr, nullptr));
    CHECK(table.count == 5 && table.limit == 5 && table.past_limit == 2);
    CHECK(side_table_past_limit_line(table, line, sizeof line));
    CHECK(
        std::strcmp(
            line,
            "gamedata/sidedata.tdf: SIDE5 to SIDE6 are past the 5 sides this game plays "
            "(limits.sides) and are left out"
        ) == 0
    );

    const std::string six = side_sections(6, nullptr);
    CHECK(load_text(six, oa::data::limits::base_sides, table));
    CHECK(table.count == 5 && table.past_limit == 1);
    CHECK(side_table_past_limit_line(table, line, sizeof line));
    CHECK(
        std::strcmp(
            line,
            "gamedata/sidedata.tdf: SIDE5 is past the 5 sides this game plays (limits.sides) and "
            "is left out"
        ) == 0
    );

    // A rectangle missing from a section the limit already left out still loads.
    const std::string broken = side_sections(6, "METALBAR");
    CHECK(load_text(broken, oa::data::limits::base_sides, table));
    CHECK(table.count == 5 && table.past_limit == 1);

    const std::string wide = side_sections(33, nullptr);
    CHECK(load_text(wide, static_cast<uint32_t>(side_table_capacity), table));
    CHECK(table.count == 32 && table.limit == 32 && table.past_limit == 1);
}

} // namespace

int main() {
    test_load_move_classes();
    test_load_sound_categories();
    test_load_side_data();
    test_side_files();
    test_side_limit();
    test_load_locale_table();
    test_category_registry_mask();
    test_category_mask_width();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    return 0;
}
