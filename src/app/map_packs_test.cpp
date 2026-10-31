// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Installed map packs: two packs list four maps, sorted, and a refresh reads
// a manifest again only when that pack's key changed. A broken manifest is
// named and left out. Removing a folder drops its maps. The folders an
// install is checked against: the game's own without the mod, and the
// copied expansions the pack needs. With the game's data, a map that uses
// only its own feature fits the base game, and one that defines
// DragonsTeeth does not; a map placing a feature an expansion's archive
// defines fits only with that expansion laid over, one placing a feature
// nothing defines is refused naming it, and Core Contingency's
// ArchiAquaOre1 is found in its own archive beside the base game's.

#include "map_packs.hpp"

#include "oa/app/game_directory.hpp"
#include "oa/app/package_install/origin.hpp"
#include "oa/app/user_folder.hpp"
#include "oa/base/sha256.hpp"
#include "oa/data/content_bundles.hpp"
#include "oa/data/map_fit/map_fit.hpp"
#include "oa/data/map_pack/manifest.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/formats/tnt.hpp"
#include "oa/test/check.hpp"
#include "oa/test/game_data.hpp"
#include "oa/test/scratch_directory.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace pack = oa::data::map_pack;
using oa::app::MapPacks;
using oa::app::PackMap;

/// A scratch folder, deleted when the test ends.
class Scratch {
  public:

    Scratch() : path_(oa::test::make_scratch_directory("oa-app-map-packs-")) {}

    ~Scratch() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }

    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;

    [[nodiscard]] const fs::path& path() const { return path_; }

  private:

    fs::path path_;
};

/// Writes a file, making its folders.
///
/// @param file the file
/// @param bytes its bytes
void write_bytes(const fs::path& file, std::string_view bytes) {
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    OA_CHECK(static_cast<bool>(out));
}

/// A catalogue origin record whose key is the digest given.
///
/// @param folder the pack's folder
/// @param registry the registry
/// @param release the release
/// @param digest the pack's key
void write_origin(
    const fs::path& folder,
    std::string_view registry,
    int64_t release,
    const oa::base::sha256::Digest& digest
) {
    oa::app::package_install::Origin origin{};
    origin.kind = oa::app::package_install::OriginKind::catalogue;
    origin.registry = std::string(registry);
    origin.catalogue_id = folder.filename().string();
    origin.release = release;
    origin.sha256 = digest;
    origin.installed = "2006-01-02";
    write_bytes(folder / ".oa-origin.yaml", oa::app::package_install::origin_text(origin));
}

/// One map of a pack written for the index.
struct Listed {
    std::string stem{};
    std::string title{};
    int32_t players{};
    std::string size{};
    std::string terrain{};
    std::string preview{};
};

/// Writes a pack folder and its origin record.
///
/// @param folder the folder
/// @param id the pack's id
/// @param name the pack's name
/// @param version the pack's version
/// @param registry the catalogue registry
/// @param release the catalogue release
/// @param digest the origin's SHA-256
/// @param maps the maps
void write_pack(
    const fs::path& folder,
    std::string_view id,
    std::string_view name,
    std::string_view version,
    std::string_view registry,
    int64_t release,
    const oa::base::sha256::Digest& digest,
    const std::vector<Listed>& maps
) {
    pack::Manifest manifest;
    manifest.format = pack::format_version;
    manifest.id = std::string(id);
    manifest.name = std::string(name);
    manifest.version = std::string(version);
    manifest.author = {"Ridge", {}};
    manifest.packaging = {1, "2026-10-11", "Open Annihilation"};
    manifest.requires_base = "ta-3.1c";
    for (const Listed& listed : maps) {
        pack::MapEntry entry;
        entry.stem = listed.stem;
        entry.title = listed.title;
        entry.players = listed.players;
        entry.size = listed.size;
        entry.preview = listed.preview;
        entry.files = {"maps/" + listed.stem + ".ota", "maps/" + listed.stem + ".tnt"};
        manifest.maps.push_back(std::move(entry));
        write_bytes(folder / "maps" / (listed.stem + ".ota"), "ota\n");
        write_bytes(folder / "maps" / (listed.stem + ".tnt"), listed.terrain);
        write_bytes(folder / listed.preview, "preview\n");
    }
    write_bytes(folder / "oamap.yaml", pack::write_manifest(manifest));
    write_origin(folder, registry, release, digest);
}

/// Counts a manifest the index parses.
///
/// @param context the count
void count_read(void* context) {
    ++*static_cast<int*>(context);
}

void test_index(const Scratch& scratch) {
    const fs::path maps = scratch.path() / "Maps";
    const uint8_t mark_r[] = {'r'};
    const uint8_t mark_h[] = {'h'};
    const uint8_t mark_next[] = {'R'};
    const auto ridge_key = oa::base::sha256::digest_of(mark_r);
    const auto harbour_key = oa::base::sha256::digest_of(mark_h);
    const auto ridge_next = oa::base::sha256::digest_of(mark_next);
    write_pack(
        maps / "ridge",
        "ridge",
        "Ridge Pack",
        "1.0",
        "ridge",
        3,
        ridge_key,
        {{"north", "North Shore", 2, "4x4", std::string(40, 'n'), "previews/north.png"},
         {"south", "South Shore", 4, "8x8", std::string(80, 's'), "previews/south.png"}}
    );
    write_pack(
        maps / "harbour",
        "harbour",
        "Harbour Pack",
        "2.0",
        "harbour",
        5,
        harbour_key,
        {{"bay", "The Bay", 2, "1x1", std::string(10, 'b'), "previews/bay.png"},
         {"cove", "The Cove", 6, "2x2", std::string(20, 'c'), "previews/cove.png"}}
    );
    write_bytes(maps / ".hidden" / "oamap.yaml", "hidden\n");

    int reads = 0;
    MapPacks packs(maps);
    packs.set_manifest_watch(&count_read, &reads);
    const auto listed = packs.maps();
    OA_CHECK(listed.size() == 4);
    OA_CHECK(reads == 2);
    const char* names[] = {"bay@harbour", "cove@harbour", "north@ridge", "south@ridge"};
    for (int index = 0; index < 4; ++index)
        OA_CHECK(listed[static_cast<std::size_t>(index)].name == names[index]);
    const PackMap* north = packs.find("north@ridge");
    OA_CHECK(north != nullptr);
    if (north != nullptr) {
        OA_CHECK(north->title == "North Shore");
        OA_CHECK(north->players == 2);
        OA_CHECK(north->size == "4x4");
        OA_CHECK(north->terrain_bytes == 40);
        OA_CHECK(north->preview.filename() == "north.png");
        OA_CHECK(north->pack_name == "Ridge Pack");
        OA_CHECK(north->registry == "ridge");
        OA_CHECK(north->release == 3);
        OA_CHECK(north->sha256.size() == oa::base::sha256::hex_size);
    }
    const PackMap* cove = packs.find("cove@harbour");
    OA_CHECK(cove != nullptr && cove->players == 6 && cove->terrain_bytes == 20);
    OA_CHECK(cove != nullptr && cove->size == "2x2" && cove->title == "The Cove");
    OA_CHECK(cove != nullptr && cove->preview.filename() == "cove.png");
    OA_CHECK(packs.find("no-such@ridge") == nullptr);
    OA_CHECK(packs.problems().empty());

    write_pack(
        maps / "ridge",
        "ridge",
        "Ridge Pack",
        "1.0",
        "ridge",
        3,
        ridge_key,
        {{"north", "North Changed", 2, "4x4", std::string(40, 'n'), "previews/north.png"},
         {"south", "South Shore", 4, "8x8", std::string(80, 's'), "previews/south.png"}}
    );
    write_pack(
        maps / "harbour",
        "harbour",
        "Harbour Pack",
        "2.0",
        "harbour",
        5,
        harbour_key,
        {{"bay", "Bay Changed", 2, "1x1", std::string(10, 'b'), "previews/bay.png"},
         {"cove", "Cove Changed", 6, "2x2", std::string(20, 'c'), "previews/cove.png"}}
    );
    write_origin(maps / "ridge", "ridge", 4, ridge_next);
    packs.refresh();
    OA_CHECK(reads == 3);
    const PackMap* reread = packs.find("north@ridge");
    const PackMap* kept = packs.find("bay@harbour");
    OA_CHECK(reread != nullptr && reread->title == "North Changed" && reread->release == 4);
    OA_CHECK(kept != nullptr && kept->title == "The Bay");

    write_bytes(maps / "broken" / "oamap.yaml", "this is not a map pack\n");
    packs.refresh();
    bool named = false;
    for (const std::string& problem : packs.problems())
        if (problem.starts_with("broken:"))
            named = true;
    OA_CHECK(named);
    OA_CHECK(packs.find("north@ridge") != nullptr);
    OA_CHECK(packs.maps().size() == 4);

    fs::remove_all(maps / "harbour");
    packs.refresh();
    OA_CHECK(packs.maps().size() == 2);
    OA_CHECK(packs.find("bay@harbour") == nullptr);
    OA_CHECK(packs.find("north@ridge") != nullptr && packs.find("south@ridge") != nullptr);
}

/// Writes a little-endian 32-bit word.
///
/// @param bytes the buffer
/// @param at the offset
/// @param value the word
void put32(std::vector<uint8_t>& bytes, std::size_t at, uint32_t value) {
    bytes[at] = static_cast<uint8_t>(value);
    bytes[at + 1] = static_cast<uint8_t>(value >> 8U);
    bytes[at + 2] = static_cast<uint8_t>(value >> 16U);
    bytes[at + 3] = static_cast<uint8_t>(value >> 24U);
}

/// Writes a little-endian 16-bit word.
///
/// @param bytes the buffer
/// @param at the offset
/// @param value the word
void put16(std::vector<uint8_t>& bytes, std::size_t at, uint16_t value) {
    bytes[at] = static_cast<uint8_t>(value);
    bytes[at + 1] = static_cast<uint8_t>(value >> 8U);
}

/// Builds a 4 by 4 map whose one feature record carries a name.
///
/// @param name the feature name
/// @return the map bytes
std::vector<uint8_t> tnt_named(std::string_view name) {
    namespace tnt = oa::formats::tnt;
    constexpr std::size_t tile_map_at = 64;
    constexpr std::size_t attributes_at = 72;
    constexpr std::size_t tiles_at = 136;
    const auto features_at = tiles_at + 2 * tnt::layout::tile_bytes;
    std::vector<uint8_t> bytes(features_at + tnt::layout::feature_record_bytes);
    put32(bytes, 0, static_cast<uint32_t>(tnt::Version::total_annihilation));
    put32(bytes, 4, 4);
    put32(bytes, 8, 4);
    put32(bytes, 12, static_cast<uint32_t>(tile_map_at));
    put32(bytes, 16, static_cast<uint32_t>(attributes_at));
    put32(bytes, 20, static_cast<uint32_t>(tiles_at));
    put32(bytes, 24, 2);
    put32(bytes, 28, 1);
    put32(bytes, 32, static_cast<uint32_t>(features_at));
    put32(bytes, 36, 17);
    put32(bytes, 40, 0);
    put32(bytes, 44, 0);
    put16(bytes, tile_map_at, 0);
    put16(bytes, tile_map_at + 2, 1);
    put16(bytes, tile_map_at + 4, 1);
    put16(bytes, tile_map_at + 6, 0);
    const auto at = features_at + offsetof(tnt::FeatureDiskRecord, name);
    const auto count = std::min(name.size(), std::size_t{127});
    for (std::size_t character = 0; character < count; ++character)
        bytes[at + character] = static_cast<uint8_t>(name[character]);
    return bytes;
}

/// The stem of a listed GAF.
///
/// @param path the listed path
/// @return the stem
std::string gaf_stem(std::string_view path) {
    const auto slash = path.find_last_of('/');
    auto file = slash == std::string_view::npos ? path : path.substr(slash + 1);
    constexpr std::string_view suffix = ".gaf";
    if (file.size() > suffix.size() && file.ends_with(suffix))
        file.remove_suffix(suffix.size());
    return std::string(file);
}

/// Tells whether a stem is letters, digits, '_' or '-'.
///
/// @param stem the stem
/// @return true when every character is one of those
bool plain_stem(std::string_view stem) {
    if (stem.empty())
        return false;
    for (const unsigned char byte : stem) {
        const bool ok = (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') ||
                        byte == '_' || byte == '-';
        if (!ok)
            return false;
    }
    return true;
}

/// The OTA that places one feature.
///
/// @param feature the feature's name
/// @return the OTA text
std::string ota_placing(std::string_view feature) {
    return "[GlobalHeader]\n"
           "{\n"
           "[Schema 0]\n"
           "{\n"
           "Type=Network 1;\n"
           "[features]\n"
           "{\n"
           "[feature0]\n"
           "{\n"
           "Featurename=" +
           std::string(feature) +
           ";\n"
           "XPos=1;\n"
           "ZPos=1;\n"
           "}\n"
           "}\n"
           "}\n"
           "}\n";
}

/// A feature TDF. The clashing one also defines DragonsTeeth.
///
/// @param stem the GAF stem the marker names
/// @param clash true to define DragonsTeeth as well
/// @return the TDF text
std::string marker_tdf(std::string_view stem, bool clash) {
    std::string text = "[zz ridge marker]\n{\nfilename=" + std::string(stem) + ";\n}\n";
    if (clash)
        text += "[DragonsTeeth]\n{\nfilename=" + std::string(stem) + ";\n}\n";
    return text;
}

/// Writes a pack folder's map that places one feature it does not define.
///
/// @param folder the pack's folder
/// @param stem the map's stem
/// @param feature the feature's name
/// @return the map's manifest entry
pack::MapEntry
write_placing_map(const fs::path& folder, const std::string& stem, std::string_view feature) {
    write_bytes(folder / "maps" / (stem + ".ota"), ota_placing(feature));
    const auto terrain = tnt_named(feature);
    write_bytes(
        folder / "maps" / (stem + ".tnt"),
        std::string_view(reinterpret_cast<const char*>(terrain.data()), terrain.size())
    );
    pack::MapEntry map;
    map.stem = stem;
    map.title = stem;
    map.players = 2;
    map.size = "4x4";
    map.files = {"maps/" + stem + ".ota", "maps/" + stem + ".tnt"};
    return map;
}

/// Tells whether a fit names a feature that is defined nowhere.
///
/// @param checked the fit
/// @param feature the feature's name
/// @return true when one failure says so
bool undefined(const oa::data::map_fit::Fit& checked, std::string_view feature) {
    for (const auto& failure : checked.failures) {
        const std::string sentence = oa::data::map_fit::describe(failure);
        if (sentence.find(feature) != std::string::npos &&
            sentence.find("defined nowhere") != std::string::npos)
            return true;
    }
    return false;
}

/// Prints each failure of a fit.
///
/// @param label what was checked
/// @param checked the fit
void print_failures(std::string_view label, const oa::data::map_fit::Fit& checked) {
    for (const auto& failure : checked.failures)
        std::fprintf(
            stderr,
            "%.*s: %s\n",
            static_cast<int>(label.size()),
            label.data(),
            oa::data::map_fit::describe(failure).c_str()
        );
}

/// A map whose feature an expansion defines fits once that expansion's
/// folder is laid over the base game, and one whose feature nothing defines
/// is refused, naming it.
///
/// @param game_dir the game's folder
/// @param gaf the stem of an animation the base game holds
void test_content_fit(const fs::path& game_dir, const std::string& gaf) {
    Scratch scratch;
    const fs::path expansion = scratch.path() / "Expansions" / "totala" / "ridge";
    const std::string tdf = "[zz ridge ore]\n{\nfilename=" + gaf + ";\n}\n";
    const oa::HpiWriteFile ore{"features/ridge/ore.tdf", {tdf.begin(), tdf.end()}};
    const std::vector<uint8_t> archive = oa::write_hpi(std::span<const oa::HpiWriteFile>(&ore, 1));
    write_bytes(
        expansion / "ridgeore.ufo",
        std::string_view(reinterpret_cast<const char*>(archive.data()), archive.size())
    );
    const fs::path folder = scratch.path() / "ridge-pack";
    const pack::MapEntry east = write_placing_map(folder, "east", "zz ridge ore");
    const pack::MapEntry west = write_placing_map(folder, "west", "zz nowhere ore");

    const oa::data::map_fit::Fit alone =
        oa::app::check_base_game_fit(game_dir, folder, east, "ridge-pack");
    OA_CHECK(undefined(alone, "zz ridge ore"));
    const std::vector<fs::path> just_the_game{game_dir};
    OA_CHECK(undefined(
        oa::app::check_content_fit(just_the_game, folder, east, "ridge-pack"), "zz ridge ore"
    ));
    const std::vector<fs::path> with_expansion{expansion, game_dir};
    const oa::data::map_fit::Fit fitted =
        oa::app::check_content_fit(with_expansion, folder, east, "ridge-pack");
    print_failures("east", fitted);
    OA_CHECK(fitted.fits());
    const oa::data::map_fit::Fit missing =
        oa::app::check_content_fit(with_expansion, folder, west, "ridge-pack");
    OA_CHECK(!missing.fits() && undefined(missing, "zz nowhere ore"));

    // Core Contingency's water metal, which three of Cavedog's maps place,
    // from the game's own archives laid out as the player's copies are:
    // the base game's in one folder and the expansion's in another.
    const fs::path base = scratch.path() / "Games" / "totala";
    const fs::path contingency = scratch.path() / "Expansions" / "totala" / "core-contingency";
    fs::create_directories(base);
    fs::create_directories(contingency);
    bool linked = true;
    const auto link_into = [&](std::string_view name, const fs::path& into) {
        const auto found = oa::app::entry_without_case(game_dir, name);
        std::error_code error;
        if (found)
            fs::create_hard_link(*found, into / std::string(name), error);
        linked = linked && found && !error;
    };
    for (const std::string_view name : {"totala1.hpi", "totala2.hpi", "rev31.gp3"})
        link_into(name, base);
    link_into("ccdata.ccx", contingency);
    if (!linked) {
        std::printf("skipped Core Contingency's features: the game's archives cannot be linked\n");
        return;
    }
    const pack::MapEntry isle = write_placing_map(folder, "isle", "ArchiAquaOre1");
    OA_CHECK(
        undefined(oa::app::check_base_game_fit(base, folder, isle, "ridge-pack"), "ArchiAquaOre1")
    );
    const std::vector<fs::path> copies{contingency, base};
    const oa::data::map_fit::Fit watered =
        oa::app::check_content_fit(copies, folder, isle, "ridge-pack");
    print_failures("isle", watered);
    OA_CHECK(watered.fits());
}

void test_base_fit() {
    const fs::path game_dir =
        oa::test::require_game_directory("a map checked against the base game");
    const oa::app::GameInstall install = oa::app::inspect_game_install(game_dir);
    OA_CHECK(oa::app::usable(install));
    if (!oa::app::usable(install))
        return;
    oa::AssetStore store(install.folders);
    for (const fs::path& archive : install.archives) {
        std::string error;
        if (!store.try_mount(archive, &error)) {
            std::fprintf(stderr, "mount failed: %s\n", error.c_str());
            OA_CHECK(false);
            return;
        }
    }
    const auto gafs = store.list_effective("anims", ".gaf", false);
    std::string stem;
    for (const std::string& path : gafs) {
        const std::string candidate = gaf_stem(path);
        if (plain_stem(candidate)) {
            stem = candidate;
            break;
        }
    }
    OA_CHECK(!stem.empty());
    if (stem.empty())
        return;

    Scratch scratch;
    const fs::path folder = scratch.path() / "ridge-pack";
    constexpr std::string_view feature = "zz ridge marker";
    const auto tnt = tnt_named(feature);
    const std::string ota = ota_placing(feature);
    const auto write_map = [&](std::string_view map_stem, std::string_view tdf, bool clash) {
        write_bytes(folder / "maps" / (std::string(map_stem) + ".ota"), ota);
        std::ofstream terrain(
            folder / "maps" / (std::string(map_stem) + ".tnt"), std::ios::binary | std::ios::trunc
        );
        terrain.write(
            reinterpret_cast<const char*>(tnt.data()), static_cast<std::streamsize>(tnt.size())
        );
        OA_CHECK(static_cast<bool>(terrain));
        write_bytes(folder / tdf, marker_tdf(stem, clash));
    };
    write_map("north", "features/ridgepack/marker.tdf", false);
    write_map("south", "features/ridgepack/clash.tdf", true);

    const auto entry = [](std::string stem, std::string tdf) {
        pack::MapEntry map;
        map.stem = std::move(stem);
        map.title = map.stem;
        map.players = 2;
        map.size = "4x4";
        map.files = {
            "maps/" + map.stem + ".ota",
            "maps/" + map.stem + ".tnt",
            std::move(tdf),
        };
        return map;
    };
    const pack::MapEntry passing = entry("north", "features/ridgepack/marker.tdf");
    const oa::data::map_fit::Fit fitted =
        oa::app::check_base_game_fit(game_dir, folder, passing, "ridge-pack");
    if (!fitted.fits()) {
        for (const auto& failure : fitted.failures)
            std::fprintf(stderr, "fitting: %s\n", oa::data::map_fit::describe(failure).c_str());
    }
    OA_CHECK(fitted.fits());

    const pack::MapEntry clashing = entry("south", "features/ridgepack/clash.tdf");
    const oa::data::map_fit::Fit clash =
        oa::app::check_base_game_fit(game_dir, folder, clashing, "ridge-pack");
    bool redefined = false;
    for (const auto& failure : clash.failures) {
        if (failure.rule == oa::data::map_fit::Rule::new_names && failure.subject == "DragonsTeeth")
            redefined = true;
        else
            std::fprintf(stderr, "clash: %s\n", oa::data::map_fit::describe(failure).c_str());
    }
    OA_CHECK(redefined);
    test_content_fit(game_dir, stem);
}

/// Writes the record of a copied bundle that lists every file of it.
///
/// @param folder the bundle's folder
/// @param id the bundle's id
void write_copy_record(const fs::path& folder, std::string_view id) {
    const oa::data::content_bundles::Bundle* bundle = oa::data::content_bundles::find_bundle(id);
    OA_CHECK(bundle != nullptr);
    if (bundle == nullptr)
        return;
    std::string record = "oa-content 1\nbundle " + std::string(id) + "\nsource G125\n";
    for (const oa::data::content_bundles::ContentFile& file : bundle->files)
        record += "file " + std::string(file.copy_name) + "\nsize 1\nmodified 1\nsha256 " +
                  std::string(64, '0') + "\n";
    write_bytes(folder / ".oa-content", record);
}

/// The folders a map pack's install is checked against.
void test_fit_folders(const Scratch& scratch) {
    const fs::path user = scratch.path() / "player";
    const fs::path base = user / "Games" / "totala";
    const fs::path expansions = user / "Expansions" / "totala";
    const fs::path campaign = expansions / "campaign";
    const fs::path contingency = expansions / "core-contingency";
    const fs::path tactics = expansions / "battle-tactics";
    const fs::path mod = user / "Mods" / "ridge";
    for (const fs::path& folder : {base, campaign, contingency, tactics, mod})
        fs::create_directories(folder);
    write_copy_record(campaign, "campaign");
    write_copy_record(contingency, "core-contingency");
    const std::vector<std::string> none;
    const std::vector<std::string> needs_contingency{"core-contingency"};
    const std::vector<std::string> needs_tactics{"battle-tactics"};

    // The game's own folders, the mod's left out.
    const std::vector<fs::path> played{mod, contingency, campaign, base};
    OA_CHECK(
        oa::app::map_pack_fit_folders(played, mod, base, user, none) ==
        std::vector<fs::path>({contingency, campaign, base})
    );
    // An expansion the pack needs that is off, with the one it needs, before
    // the base game.
    const std::vector<fs::path> base_alone{base};
    OA_CHECK(
        oa::app::map_pack_fit_folders(base_alone, {}, base, user, needs_contingency) ==
        std::vector<fs::path>({campaign, contingency, base})
    );
    // One that is on already is not listed twice.
    OA_CHECK(
        oa::app::map_pack_fit_folders(played, mod, base, user, needs_contingency) ==
        std::vector<fs::path>({contingency, campaign, base})
    );
    // One the player has not copied (no record) is not there to look in.
    OA_CHECK(
        oa::app::map_pack_fit_folders(base_alone, {}, base, user, needs_tactics) ==
        std::vector<fs::path>({campaign, base})
    );
    // No folders listed: the game folder alone.
    OA_CHECK(
        oa::app::map_pack_fit_folders({}, {}, base, {}, needs_contingency) ==
        std::vector<fs::path>({base})
    );
}

/// The compatibility key, the catalogue's answer and the greyed reasons.
void test_list_rules() {
    const std::vector<std::string> music_then_core{"music", "core-contingency"};
    OA_CHECK(
        oa::app::map_compatibility_key("totala", music_then_core, "") == "totala+core-contingency"
    );
    OA_CHECK(
        oa::app::map_compatibility_key("totala", music_then_core, "ridge@28") ==
        "totala+core-contingency+ridge@28"
    );
    const std::vector<std::string> core_only{"core-contingency"};
    OA_CHECK(
        oa::app::map_compatibility_key("totala", core_only, "") ==
        oa::app::map_compatibility_key("totala", music_then_core, "")
    );
    OA_CHECK(oa::app::map_compatibility_key("totala-demo", music_then_core, "") == "totala-demo");
    OA_CHECK(oa::app::map_compatibility_key("", {}, "ridge@28") == "totala+ridge@28");
    OA_CHECK(oa::app::legacy_compatibility_key("totala", "") == "ta-3.1c");
    OA_CHECK(oa::app::legacy_compatibility_key("", "") == "ta-3.1c");
    OA_CHECK(oa::app::legacy_compatibility_key("totala", "ridge@28") == "ridge@28");
    OA_CHECK(oa::app::legacy_compatibility_key("totala-demo", "").empty());

    const std::vector<std::string> demo_only{"totala-demo"};
    const oa::app::MapListVerdict demo = oa::app::map_list_verdict(
        demo_only, {}, "totala", {}, "", oa::app::ListedFit::fits, {}, {}
    );
    OA_CHECK(demo.block == oa::app::MapListBlock::other_game && demo.demo);
    OA_CHECK(oa::app::map_list_row_reason(demo) == "for the TA demo");
    OA_CHECK(oa::app::map_list_explanation(demo, "No Mod") == "This map is for the TA demo.");

    const oa::app::MapListVerdict full_on_demo =
        oa::app::map_list_verdict({}, {}, "totala-demo", {}, "", oa::app::ListedFit::fits, {}, {});
    OA_CHECK(
        full_on_demo.block == oa::app::MapListBlock::other_game && !full_on_demo.demo &&
        full_on_demo.name == "Total Annihilation"
    );
    OA_CHECK(oa::app::map_list_row_reason(full_on_demo) == "for Total Annihilation");

    const std::vector<std::string> totala{"totala"};
    const std::vector<std::string> tactics{"battle-tactics"};
    const oa::app::MapListVerdict needs = oa::app::map_list_verdict(
        totala, tactics, "totala", {}, "", oa::app::ListedFit::fits, {}, {}
    );
    OA_CHECK(needs.block == oa::app::MapListBlock::needs_expansion);
    OA_CHECK(oa::app::map_list_row_reason(needs) == "needs Battle Tactics");
    OA_CHECK(
        oa::app::map_list_explanation(needs, "No Mod") ==
        "Needs Battle Tactics, which is off. Turn it on in the Library."
    );
    const oa::app::FileFit missed{true, false, "a feature is already defined"};
    const oa::app::MapListVerdict tactics_on = oa::app::map_list_verdict(
        totala, tactics, "totala", tactics, "", oa::app::ListedFit::fits, {}, missed
    );
    OA_CHECK(tactics_on.block == oa::app::MapListBlock::playable);
    const std::vector<std::string> music{"music"};
    const oa::app::MapListVerdict music_off = oa::app::map_list_verdict(
        totala, music, "totala", {}, "", oa::app::ListedFit::fits, {}, missed
    );
    OA_CHECK(music_off.block == oa::app::MapListBlock::playable);

    const oa::app::CompatibleFlag ridge_rows[] = {
        {"ta-3.1c", false},
        {"ridge@28", true},
    };
    const oa::app::MapListVerdict for_ridge = oa::app::map_list_verdict(
        totala, {}, "totala", {}, "", oa::app::ListedFit::unfit, ridge_rows, missed
    );
    OA_CHECK(
        for_ridge.block == oa::app::MapListBlock::unfit && for_ridge.final_note &&
        for_ridge.name == "ridge"
    );
    OA_CHECK(oa::app::map_list_row_reason(for_ridge) == "for ridge");
    OA_CHECK(oa::app::map_list_explanation(for_ridge, "No Mod") == "This map is for ridge.");

    const oa::app::MapListVerdict catalogue_miss = oa::app::map_list_verdict(
        totala, {}, "totala", {}, "", oa::app::ListedFit::unfit, {}, missed
    );
    OA_CHECK(catalogue_miss.final_note && catalogue_miss.name.empty());
    OA_CHECK(oa::app::map_list_row_reason(catalogue_miss) == "doesn't fit");
    OA_CHECK(
        oa::app::map_list_explanation(catalogue_miss, "No Mod") ==
        "Doesn't fit No Mod: Its catalogue says it does not fit No Mod."
    );

    const oa::app::MapListVerdict ask = oa::app::map_list_verdict(
        totala, {}, "totala", {}, "", oa::app::ListedFit::unchecked, {}, {}
    );
    OA_CHECK(ask.block == oa::app::MapListBlock::ask_files);
    const oa::app::MapListVerdict file_miss = oa::app::map_list_verdict(
        totala, {}, "totala", {}, "", oa::app::ListedFit::unchecked, {}, missed
    );
    OA_CHECK(
        file_miss.block == oa::app::MapListBlock::unfit && file_miss.final_note &&
        file_miss.detail == "a feature is already defined"
    );
    const oa::app::MapListVerdict provisional = oa::app::map_list_verdict(
        totala, {}, "totala", {}, "", oa::app::ListedFit::unlisted, {}, missed
    );
    OA_CHECK(provisional.block == oa::app::MapListBlock::unfit && !provisional.final_note);

    const oa::app::CompatibleFlag newer_false[] = {
        {"totala", false},
        {"ta-3.1c", true},
    };
    const std::optional<bool> newer = oa::app::compatible_answer(newer_false, "totala", {}, "");
    OA_CHECK(newer.has_value() && !*newer);
    const std::vector<std::string> extra{"core-contingency", "music"};
    const std::optional<bool> still_totala =
        oa::app::compatible_answer(newer_false, "totala", extra, "");
    OA_CHECK(still_totala.has_value() && !*still_totala);

    const oa::app::CompatibleFlag contingency[] = {
        {"totala+core-contingency", true},
        {"ta-3.1c", false},
    };
    const std::optional<bool> without = oa::app::compatible_answer(contingency, "totala", {}, "");
    OA_CHECK(without.has_value() && !*without);
    const std::vector<std::string> with_core{"core-contingency"};
    const std::optional<bool> with =
        oa::app::compatible_answer(contingency, "totala", with_core, "");
    OA_CHECK(with.has_value() && *with);

    const oa::app::CompatibleFlag longest[] = {
        {"totala", false},
        {"totala+core-contingency", true},
    };
    const std::optional<bool> specific =
        oa::app::compatible_answer(longest, "totala", with_core, "");
    OA_CHECK(specific.has_value() && *specific);
    const oa::app::CompatibleFlag absent[] = {{"ridge@28", true}};
    OA_CHECK(!oa::app::compatible_answer(absent, "totala", {}, "").has_value());

    const oa::app::CompatibleFlag music_true[] = {{"totala+music", true}};
    const std::vector<std::string> music_on{"music"};
    const std::optional<bool> music_off_answer =
        oa::app::compatible_answer(music_true, "totala", {}, "");
    const std::optional<bool> music_on_answer =
        oa::app::compatible_answer(music_true, "totala", music_on, "");
    OA_CHECK(
        music_off_answer.has_value() && *music_off_answer && music_on_answer.has_value() &&
        *music_on_answer
    );
}

/// A map a catalogue lists, judged before its pack is fetched, as GET MAP
/// asks: a pack that needs Core Contingency, published with rows for the
/// base game alone and with Core Contingency, as 10 to 1 (fixed) is.
void test_catalogue_verdict() {
    const std::vector<std::string> totala{"totala"};
    const std::vector<std::string> contingency{"core-contingency"};
    const std::vector<std::pair<std::string, bool>> rows{
        {"totala", false},
        {"totala+core-contingency", true},
        {"totala+core-contingency+ridge@28", false},
        {"ta-3.1c", true},
    };
    const std::vector<std::string> all_on{
        "campaign", "core-contingency", "battle-tactics", "music"
    };

    // It fits: Core Contingency is on.
    const oa::app::MapListVerdict fits =
        oa::app::catalogue_map_verdict(totala, contingency, rows, "totala", all_on, "", false);
    OA_CHECK(fits.block == oa::app::MapListBlock::playable);

    // Core Contingency is off, or not there at all: the map list's sentence
    // tells the player to turn it on.
    const std::vector<std::string> campaign_only{"campaign"};
    const oa::app::MapListVerdict off = oa::app::catalogue_map_verdict(
        totala, contingency, rows, "totala", campaign_only, "", false
    );
    OA_CHECK(off.block == oa::app::MapListBlock::needs_expansion);
    OA_CHECK(
        oa::app::map_list_explanation(off, "No Mod") ==
        "Needs Core Contingency, which is off. Turn it on in the Library."
    );
    // A required expansion is told even with no rows.
    const oa::app::MapListVerdict off_unlisted =
        oa::app::catalogue_map_verdict(totala, contingency, {}, "totala", {}, "", false);
    OA_CHECK(off_unlisted.block == oa::app::MapListBlock::needs_expansion);

    // The rows say it does not fit the mod played.
    const oa::app::MapListVerdict mod_miss = oa::app::catalogue_map_verdict(
        totala, contingency, rows, "totala", all_on, "ridge@28", false
    );
    OA_CHECK(mod_miss.block == oa::app::MapListBlock::unfit && mod_miss.final_note);
    OA_CHECK(
        oa::app::map_list_explanation(mod_miss, "Ridge") ==
        "Doesn't fit Ridge: Its catalogue says it does not fit Ridge."
    );
    // A base game the pack does not support.
    const oa::app::MapListVerdict demo =
        oa::app::catalogue_map_verdict(totala, {}, rows, "totala-demo", {}, "", false);
    OA_CHECK(demo.block == oa::app::MapListBlock::other_game);
    OA_CHECK(
        oa::app::map_list_explanation(demo, "No Mod") == "This map is for Total Annihilation."
    );

    // No fit data: no rows, rows that name neither this game nor its mod, or
    // a mod with no catalogue key. The pack is fetched and its files decide.
    const oa::app::MapListVerdict no_rows =
        oa::app::catalogue_map_verdict(totala, {}, {}, "totala", all_on, "", false);
    OA_CHECK(no_rows.block == oa::app::MapListBlock::ask_files);
    const std::vector<std::pair<std::string, bool>> other_mod_only{{"totala+ridge@28", false}};
    const oa::app::MapListVerdict unnamed =
        oa::app::catalogue_map_verdict(totala, {}, other_mod_only, "totala", {}, "dunes@3", false);
    OA_CHECK(unnamed.block == oa::app::MapListBlock::ask_files);
    const oa::app::MapListVerdict unkeyed =
        oa::app::catalogue_map_verdict(totala, contingency, rows, "totala", all_on, "", true);
    OA_CHECK(unkeyed.block == oa::app::MapListBlock::ask_files);
}

} // namespace

int main(int argc, char** argv) {
    if (oa::test::game_data_requested(argc, argv))
        test_base_fit();
    else {
        test_list_rules();
        test_catalogue_verdict();
        const Scratch scratch;
        test_index(scratch);
        test_fit_folders(scratch);
    }
    return oa::test::check_exit_status();
}
