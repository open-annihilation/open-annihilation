// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The data layout: the base game's names by default, and with every
// directory renamed and another unit file extension, the loaders read only
// the renamed directories, as the asset store's lookup log shows, while base
// files of the same names lie beside them.

#include "oa/data/defs/asset_files.hpp"
#include "oa/data/defs/gamedata_tables.hpp"
#include "oa/data/defs/layout.hpp"
#include "oa/data/defs/move_classes.hpp"
#include "oa/data/defs/sides.hpp"
#include "oa/data/defs/sound_categories.hpp"
#include "oa/data/defs/unit_catalog.hpp"
#include "oa/data/defs/unit_header.hpp"
#include "oa/data/defs/version.hpp"
#include "oa/data/defs/weapons.hpp"
#include "oa/formats/hpi.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace defs = oa::data::defs;

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::string folded(std::string_view text) {
    std::string result(text);
    for (auto& c : result) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (c == '\\')
            c = '/';
    }
    return result;
}

class TempDir {
  public:

    TempDir() {
        static std::atomic<int> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = fs::temp_directory_path() /
                ("oa-layout-test-" + std::to_string(stamp) + "-" + std::to_string(counter++));
        fs::create_directories(path_);
    }

    ~TempDir() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    void write(const std::string& name, std::string_view text) const {
        const auto target = path_ / name;
        fs::create_directories(target.parent_path());
        std::ofstream(target, std::ios::binary) << text;
    }

    const fs::path& path() const { return path_; }

  private:

    fs::path path_;
};

/// Restores the base layout when a test ends.
struct LayoutScope {
    explicit LayoutScope(const defs::DataLayout& layout) { defs::use_data_layout(layout); }

    ~LayoutScope() { defs::use_data_layout({}); }

    LayoutScope(const LayoutScope&) = delete;
    LayoutScope& operator=(const LayoutScope&) = delete;
};

void base_layout_is_the_games() {
    check(defs::data_layout() == defs::DataLayout{}, "the layout starts as the base game's");
    check(defs::gui_path("mainmenu.gui") == "guis/mainmenu.gui", "GUI layouts lie in guis");
    check(
        defs::data_path(defs::DataDirectory::gamedata, "sidedata.tdf") == "gamedata/sidedata.tdf",
        "side data lies in gamedata"
    );
    check(std::string_view(defs::directory_name(defs::DataDirectory::weapons)) == "Weapons", "");
    check(defs::unit_file_suffix() == ".FBI", "units are FBI files");
    check(std::string_view(defs::map_units_section()) == "units", "maps place units in units");
    check(
        defs::data_layout().build_version[0] == 3 && defs::data_layout().build_version[1] == 1,
        "units are checked against build 3.1"
    );
    check(defs::computer_player_sides() == 0, "a computer player may take every side");
}

void renamed_directories_replace_the_base_ones() {
    defs::DataLayout layout;
    layout.directories = {"unitsX", "weaponX", "gamedatX", "aX", "guiX", "unitpicX", "downloadX"};
    layout.unit_extension = "UNX";
    layout.map_units_section = "unitsX";
    const LayoutScope scope(layout);
    check(defs::gui_path("mainmenu.gui") == "guiX/mainmenu.gui", "a renamed GUI directory");

    TempDir dir;
    const std::string moveinfo = "[CLASS0]{name=TANKSH2;footprintx=2;footprintz=2;}";
    const std::string sidedata =
        "[SIDE0]{name=ARM;nameprefix=ARM;commander=ARMCOM;}[CANBUILD]{[ARMCOM]{canbuild1=ARMCOM;}}";
    const std::string unit = "[UNITINFO]{unitname=ARMCOM;name=Commander;Version=3.1;}";
    const std::string weapon = "[GUN]{id=1;name=Gun;}";
    // The base folders, which a renamed layout never reads.
    for (const char* base : {"units/armcom.fbi", "units/armcom.unx"})
        dir.write(base, unit);
    dir.write("Weapons/gun.tdf", weapon);
    for (const char* name : {"moveinfo", "sidedata", "sound", "los", "version"})
        dir.write(std::string("gamedata/") + name + ".tdf", "[BASE]{}");
    dir.write("download/armcom.tdf", "[MENUENTRY1]{UNITMENU=ARMCOM;UNITNAME=ARMCOM;}");
    // The renamed folders.
    dir.write("gamedatX/moveinfo.tdf", moveinfo);
    dir.write("gamedatX/sidedata.tdf", sidedata);
    dir.write("gamedatX/sound.tdf", "[ARM_COMMANDER]{select1=ok;}");
    dir.write("gamedatX/los.tdf", "[TABLE1]{line1=0;}");
    dir.write("gamedatX/version.tdf", "[VERSION]{GPFVersion=v3.0;}");
    dir.write("unitsX/armcom.unx", unit);
    dir.write("unitsX/armcom.fbi", unit);
    dir.write("weaponX/gun.tdf", weapon);
    dir.write("downloadX/armcom.tdf", "[MENUENTRY1]{UNITMENU=ARMCOM;UNITNAME=ARMCOM;}");

    oa::AssetStore store(dir.path());
    std::vector<std::string> seen;
    store.observe_lookups({&seen, [](void* context, std::string_view name) {
                               static_cast<std::vector<std::string>*>(context)->emplace_back(name);
                           }});
    const auto files = defs::asset_store_files(&store);

    defs::MoveClassTable moves{};
    defs::move_class_table_init(&moves);
    check(defs::load_move_classes(&files, &moves, nullptr, nullptr), "MOVEINFO loads");
    auto sides = std::make_unique<defs::SideTable>();
    (void)defs::load_side_data(&files, sides.get(), nullptr, nullptr);
    defs::SoundCategoryTable sounds{};
    (void)defs::load_sound_categories(&files, &sounds, nullptr);
    defs::sound_category_table_free(&sounds);
    (void)defs::load_gamedata_tables(&files, nullptr, {});
    (void)defs::revision_gpf_mismatch(&files, nullptr);
    auto weapons = std::make_unique<defs::WeaponTable>();
    defs::weapon_table_init(weapons.get());
    check(defs::load_weapon_defs(&files, weapons.get(), nullptr) == 1, "one weapon file loads");
    defs::weapon_table_free(weapons.get());
    defs::WeaponTdfSet weapon_files{};
    defs::weapon_tdf_set_init(&weapon_files);
    check(
        defs::load_weapon_tdf_set(&files, nullptr, false, &weapon_files) && weapon_files.count == 1,
        "the weapon file set holds the renamed directory's file"
    );

    std::vector<std::string> unit_files;
    files.list(
        files.context,
        defs::directory_name(defs::DataDirectory::units),
        defs::unit_extension(),
        [](void* user, const char* name) {
            static_cast<std::vector<std::string>*>(user)->emplace_back(name);
        },
        &unit_files
    );
    check(
        unit_files.size() == 1 && folded(unit_files.front()) == "armcom.unx",
        "only files of the layout's unit extension are units"
    );
    defs::UnitDefTables tables{};
    defs::unit_def_tables_init(&tables);
    check(defs::unit_def_tables_allocate(&tables, 2), "the unit table is allocated");
    char path[defs::path_capacity];
    defs::build_variant_path(
        &files,
        path,
        sizeof path,
        defs::directory_name(defs::DataDirectory::units),
        "armcom",
        defs::unit_extension(),
        nullptr
    );
    const defs::UnitHeaderSources header_sources{"", &weapon_files, 3, 1, false, false};
    bool refused = false;
    check(
        defs::load_unit_header(&files, path, tables.records[1], header_sources, &refused),
        "the unit header loads from the renamed directory"
    );
    (void)defs::load_build_lists(&files, nullptr, &tables);
    (void)defs::load_download_menu(&files, nullptr, &tables);
    defs::unit_def_tables_free(&tables);
    defs::weapon_tdf_set_free(&weapon_files);

    const std::string_view base_names[] = {
        "units/", "weapons/", "gamedata/", "ai/", "guis/", "unitpics/", "download/"
    };
    for (const auto& name : seen) {
        const auto key = folded(name);
        for (const auto base : base_names)
            check(
                !key.starts_with(base) && key != base.substr(0, base.size() - 1),
                "a base directory was looked up: " + name
            );
    }
    for (const char* renamed :
         {"gamedatx/moveinfo.tdf",
          "gamedatx/sidedata.tdf",
          "weaponx",
          "downloadx",
          "unitsx/armcom.unx"})
        check(
            std::any_of(
                seen.begin(),
                seen.end(),
                [&](const std::string& name) { return folded(name) == renamed; }
            ),
            std::string("the renamed path was looked up: ") + renamed
        );
}

} // namespace

/// The side slots' names: with two, every side but the first takes the
/// second; with more, each side its own, and a side past the last the last.
void side_names_name_each_side() {
    check(std::string_view(defs::side_name(0)) == "Arm", "side 0 is the Arm");
    check(std::string_view(defs::side_name(1)) == "Core", "side 1 is the Core");
    check(std::string_view(defs::side_name(2)) == "Core", "with two names, side 2 is the Core");
    defs::DataLayout layout;
    layout.side_names = {"North", "South", "East"};
    const LayoutScope scope(layout);
    check(std::string_view(defs::side_name(0)) == "North", "three names: side 0");
    check(std::string_view(defs::side_name(2)) == "East", "three names: side 2 has its own");
    check(std::string_view(defs::side_name(4)) == "East", "three names: side 4 takes the last");
}

int main() {
    try {
        base_layout_is_the_games();
        side_names_name_each_side();
        base_layout_is_the_games();
        renamed_directories_replace_the_base_ones();
        base_layout_is_the_games();
    } catch (const std::exception& error) {
        std::cerr << "unexpected exception: " << error.what() << '\n';
        return 1;
    }
    if (failures != 0) {
        std::cerr << failures << " layout check(s) failed\n";
        return 1;
    }
    std::cout << "data layout tests passed\n";
    return 0;
}
