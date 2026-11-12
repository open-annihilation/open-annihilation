// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The composed mount of a chosen base game and its expansions: the same
// archives, in the same order, as a scan of one folder that holds those
// files. Battle Tactics off keeps totala4.hpi inside the first ten plain
// archives. The base game alone mounts its own archives. Music and movies
// are found in the bundle folders, and a name's case does not matter.
// Expansions with no mod report no mod folder. A mod folder is reported
// on its own, including while an expansion is mounted with it. A copy that
// stopped part way is not mounted, and what is missing is named. Only the
// bundle table's archives are read from a bundle's folder: a stray archive
// there, as in the 0.9.0 incident, is not mounted, and the content check
// names it. A folder that is not Open Annihilation's copy is checked by the
// bundle hashes.

#include "oa/app/content_copy.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/base/sha256.hpp"
#include "oa/audio/music_disc.hpp"
#include "oa/data/content_bundles.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/formats/tdf.hpp"
#include "oa/test/check.hpp"
#include "oa/test/scratch_directory.hpp"
#include "oa/ui/engine_settings.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace oa::app;
namespace bundles = oa::data::content_bundles;

constexpr std::string_view kEveryExpansion = "totala:campaign,core-contingency,battle-tactics,"
                                             "music,cavedog-units,cavedog-tilesets,boneyards";

/// Compares two strings without regard to ASCII case.
///
/// @param left one string
/// @param right the other
/// @return true when they match
[[nodiscard]] bool same_ascii(std::string_view left, std::string_view right) {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index)
        if (std::tolower(static_cast<unsigned char>(left[index])) !=
            std::tolower(static_cast<unsigned char>(right[index])))
            return false;
    return true;
}

/// Reports whether a copy name is an archive discovery would mount.
///
/// @param name the copy name
/// @return true for an archive suffix
[[nodiscard]] bool archive_name(std::string_view name) {
    const auto dot = name.rfind('.');
    if (dot == std::string_view::npos)
        return false;
    const std::string_view extension = name.substr(dot);
    return same_ascii(extension, ".hpi") || same_ascii(extension, ".ufo") ||
           same_ascii(extension, ".ccx") || same_ascii(extension, ".gp3");
}

/// The bytes of one tiny archive.
///
/// @return a mountable archive
[[nodiscard]] std::vector<uint8_t> archive_bytes() {
    const std::vector<uint8_t> text{'o', 'k'};
    const oa::HpiWriteFile file{"readme.txt", text};
    return oa::write_hpi(std::span<const oa::HpiWriteFile>(&file, 1));
}

/// Writes bytes to a file, creating its folder.
///
/// @param path the file
/// @param bytes its contents
void write_bytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(
        reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
    );
    OA_CHECK(static_cast<bool>(out));
}

/// The folder one bundle is stored in under a player's folder.
///
/// @param user the player's folder
/// @param bundle the bundle
/// @return Games/<id> or Expansions/<base>/<id>
[[nodiscard]] fs::path managed_folder(const fs::path& user, const bundles::Bundle& bundle) {
    const fs::path id{std::string(bundle.id)};
    if (bundle.kind == bundles::Kind::base_game)
        return user / std::string(oa::app::managed_games_folder) / id;
    return user / std::string(oa::app::managed_expansions_folder) / std::string(bundle.extends[0]) /
           id;
}

/// Writes a record that lists the first files of a bundle, as a copy that
/// stopped after them leaves it, or every file.
///
/// @param folder the bundle folder
/// @param bundle the bundle
/// @param count how many of its files the record lists; every file by default
void write_record(
    const fs::path& folder,
    const bundles::Bundle& bundle,
    std::size_t count = std::numeric_limits<std::size_t>::max()
) {
    fs::create_directories(folder);
    std::ofstream out(folder / ".oa-content");
    const std::string hash(64, '0');
    out << "oa-content 1\n";
    out << "bundle " << bundle.id << '\n';
    out << "source G05\n";
    for (const bundles::ContentFile& file :
         bundle.files.first(std::min(count, bundle.files.size()))) {
        out << "file " << file.copy_name << '\n';
        out << "size 1\n";
        out << "modified 1\n";
        out << "sha256 " << hash << '\n';
    }
    OA_CHECK(static_cast<bool>(out));
}

/// Writes one bundle's files under `root`, under each file's copy name.
///
/// @param root the folder
/// @param bundle the bundle
/// @param archive the bytes of each archive
void place_bundle(
    const fs::path& root, const bundles::Bundle& bundle, const std::vector<uint8_t>& archive
) {
    for (const bundles::ContentFile& file : bundle.files) {
        if (archive_name(file.copy_name))
            write_bytes(root / path_from_utf8(file.copy_name), archive);
        else
            write_bytes(root / path_from_utf8(file.copy_name), std::vector<uint8_t>{'x'});
    }
}

/// The file names discovery mounted, in mount order.
///
/// @param archives the mounted archives
/// @return each archive's file name
[[nodiscard]] std::vector<std::string> file_names(const std::vector<fs::path>& archives) {
    std::vector<std::string> names;
    names.reserve(archives.size());
    for (const fs::path& archive : archives)
        names.push_back(path_to_utf8(archive.filename()));
    return names;
}

/// The file names a scan of one folder mounts.
///
/// @param folder the folder
/// @return the mounted file names, in mount order
[[nodiscard]] std::vector<std::string> scanned_names(const fs::path& folder) {
    oa::AssetStore store(folder);
    for (const auto& outcome : store.discover(discovery_plan_of(nullptr)))
        std::ignore = outcome.mounted;
    std::vector<std::string> names;
    const auto mounted = store.mount_paths();
    names.reserve(mounted.size());
    for (const fs::path& archive : mounted)
        names.push_back(path_to_utf8(archive.filename()));
    return names;
}

/// The plain archives among the mounted names, in mount order.
///
/// @param names the mounted file names
/// @return those whose suffix is .hpi
[[nodiscard]] std::vector<std::string> hpi_names(const std::vector<std::string>& names) {
    std::vector<std::string> hpis;
    for (const std::string& name : names)
        if (name.size() >= 4 && same_ascii(std::string_view(name).substr(name.size() - 4), ".hpi"))
            hpis.push_back(name);
    return hpis;
}

/// Mounts `content` from `user`.
///
/// @param user the player's folder
/// @param content `--content`; empty uses `values`
/// @param values the preferences
/// @param mod_dir a mod folder; empty for none
/// @return the composed mount
[[nodiscard]] std::optional<GameDirectory> mount_of(
    const fs::path& user,
    std::string_view content,
    const oa::platform::preferences::Values& values,
    const fs::path& mod_dir = {}
) {
    ManagedMount request{};
    request.user_folder = user;
    request.content = content;
    request.mod_dir = mod_dir;
    return compose_managed_content(request, values);
}

/// True when `names` holds `wanted`.
///
/// @param names the names
/// @param wanted the name
/// @return true when it is there
[[nodiscard]] bool contains(const std::vector<std::string>& names, std::string_view wanted) {
    for (const std::string& name : names)
        if (name == wanted)
            return true;
    return false;
}

void check_composed_mount(const fs::path& scratch) {
    const std::vector<uint8_t> archive = archive_bytes();
    const fs::path user = scratch / "player";
    const fs::path install = scratch / "install";
    std::vector<std::string> recorded;
    for (const bundles::Bundle& bundle : bundles::bundles()) {
        if (bundle.id == "totala-demo")
            continue;
        place_bundle(install, bundle, archive);
        const fs::path folder = managed_folder(user, bundle);
        place_bundle(folder, bundle, archive);
        write_record(folder, bundle);
        recorded.emplace_back(bundle.id);
    }

    const std::vector<std::string> scanned = scanned_names(install);
    const GameInstall inspected = inspect_game_install(install);
    OA_CHECK(file_names(inspected.archives) == scanned);

    const oa::platform::preferences::Values values;
    const std::optional<GameDirectory> every = mount_of(user, kEveryExpansion, values);
    OA_CHECK(every.has_value());
    if (!every)
        return;
    OA_CHECK(every->source == GameDirectorySource::managed);
    OA_CHECK(!every->mod_layered);
    OA_CHECK(every->mod_folder.empty());
    OA_CHECK(every->profile == nullptr);
    OA_CHECK(file_names(every->archives) == scanned);
    OA_CHECK(every->folders.size() == 8);
    OA_CHECK(path_to_utf8(every->folders.back().filename()) == "totala");

    const std::vector<std::string> hpis = hpi_names(file_names(every->archives));
    const std::vector<std::string> first_ten{
        "tactics1.hpi",
        "tactics2.hpi",
        "tactics3.hpi",
        "tactics4.hpi",
        "tactics5.hpi",
        "tactics6.hpi",
        "tactics7.hpi",
        "tactics8.hpi",
        "totala1.hpi",
        "totala2.hpi",
    };
    OA_CHECK(hpis.size() == first_ten.size() + 2);
    OA_CHECK(
        hpis.size() >= first_ten.size() &&
        std::vector<std::string>(
            hpis.begin(), hpis.begin() + static_cast<std::ptrdiff_t>(first_ten.size())
        ) == first_ten
    );
    OA_CHECK(contains(hpis, "totala4.hpi") && contains(hpis, "worlds.hpi"));
    OA_CHECK(!hpis.empty() && hpis.back() == "worlds.hpi");

    const std::optional<GameDirectory> without_tactics = mount_of(
        user, "totala:campaign,core-contingency,music,cavedog-units,cavedog-tilesets", values
    );
    OA_CHECK(without_tactics.has_value());
    if (without_tactics) {
        const std::vector<std::string> plain = hpi_names(file_names(without_tactics->archives));
        std::size_t totala4 = plain.size();
        for (std::size_t index = 0; index < plain.size(); ++index)
            if (plain[index] == "totala4.hpi")
                totala4 = index;
        OA_CHECK(totala4 < 10);
        OA_CHECK(!contains(plain, "tactics1.hpi"));
    }

    const std::optional<GameDirectory> base = mount_of(user, "totala", values);
    OA_CHECK(base.has_value());
    if (base) {
        OA_CHECK(
            file_names(base->archives) ==
            std::vector<std::string>({"rev31.gp3", "totala1.hpi", "totala2.hpi"})
        );
        OA_CHECK(roots_hold_movies(base->folders));
    }
    OA_CHECK(roots_hold_movies(every->folders));

    bool music = false;
    for (const fs::path& root : every->folders) {
        const fs::path directory = oa::audio::music_disc_directory(root);
        if (oa::audio::music_disc_present(oa::audio::music_disc_scan(directory)))
            music = true;
    }
    OA_CHECK(music);

    const fs::path mod = scratch / "ridge";
    write_bytes(mod / "rev31.gp3", archive_bytes());
    const std::optional<GameDirectory> layered = mount_of(user, "totala", values, mod);
    OA_CHECK(layered.has_value());
    if (layered) {
        bool replaced = false;
        for (const fs::path& mounted : layered->archives)
            if (path_to_utf8(mounted.filename()) == "rev31.gp3") {
                std::error_code failure;
                replaced = fs::equivalent(mounted.parent_path(), mod, failure);
            }
        OA_CHECK(replaced);
        OA_CHECK(layered->mod_layered);
        OA_CHECK(layered->mod_folder == mod);
        OA_CHECK(!layered->folders.empty() && layered->folders.front() == mod);
    }

    const std::optional<GameDirectory> campaign_and_mod =
        mount_of(user, "totala:campaign", values, mod);
    OA_CHECK(campaign_and_mod.has_value());
    if (campaign_and_mod) {
        bool campaign = false;
        for (const fs::path& root : campaign_and_mod->folders)
            if (path_to_utf8(root.filename()) == "campaign")
                campaign = true;
        OA_CHECK(campaign);
        OA_CHECK(campaign_and_mod->mod_layered);
        OA_CHECK(campaign_and_mod->mod_folder == mod);
        OA_CHECK(campaign_and_mod->folders.size() >= 3);
    }

    oa::platform::preferences::Values stored;
    stored[std::string(oa::ui::engine_settings::key::base_game)] = "totala";
    stored[std::string(oa::ui::engine_settings::key::expansions_prefix) + "totala"] =
        "campaign,battle-tactics";
    const std::optional<GameDirectory> tactics_on = mount_of(user, {}, stored);
    OA_CHECK(tactics_on.has_value());
    if (tactics_on)
        OA_CHECK(contains(file_names(tactics_on->archives), "tactics1.hpi"));
    stored[std::string(oa::ui::engine_settings::key::expansions_prefix) + "totala"] = "campaign";
    const std::optional<GameDirectory> tactics_off = mount_of(user, {}, stored);
    OA_CHECK(tactics_off.has_value());
    if (tactics_off) {
        const std::vector<std::string> plain = hpi_names(file_names(tactics_off->archives));
        OA_CHECK(!contains(plain, "tactics1.hpi"));
        OA_CHECK(contains(plain, "totala4.hpi"));
        OA_CHECK(plain.size() <= 10);
    }

    OA_CHECK(copied_content_bundles(user) == recorded);
    OA_CHECK(
        copied_content_bundles(user / std::string(oa::app::managed_games_folder) / "totala") ==
        recorded
    );

    const fs::path plain = scratch / "plain";
    fs::create_directory(plain);
    std::ofstream(plain / "totala1.hpi") << "short";
    OA_CHECK(copied_content_bundles(plain).empty());

    const fs::path partial = scratch / "partial";
    const fs::path partial_game = partial / std::string(oa::app::managed_games_folder) / "totala";
    fs::create_directories(partial_game);
    {
        std::ofstream out(partial_game / ".oa-content");
        out << "oa-content 1\n";
        out << "bundle totala\n";
        out << "source G05\n";
        out << "file rev31.gp3\n";
        out << "size 1\n";
        out << "modified 1\n";
        out << "sha256 " << std::string(64, '0') << '\n';
        OA_CHECK(static_cast<bool>(out));
    }
    OA_CHECK(copied_content_bundles(partial).empty());
}

/// A copy that stopped part way is not mounted, and the start that stops
/// there names the files that were not copied. A base game never copied,
/// and a copy whose archives are gone, are named too.
void check_unfinished_copy(const fs::path& scratch) {
    const bundles::Bundle* totala = bundles::find_bundle("totala");
    OA_CHECK(totala != nullptr);
    if (totala == nullptr)
        return;
    const oa::platform::preferences::Values values;
    ManagedMount request{};
    request.user_folder = scratch / "unfinished-player";
    const fs::path folder = managed_folder(request.user_folder, *totala);
    const std::string where = path_to_utf8(folder);
    OA_CHECK(!compose_managed_content(request, values).has_value());
    OA_CHECK(
        managed_copy_problem(request, values) ==
        "Open Annihilation has not copied Total Annihilation 3.1c into " + where +
            ". Start Open Annihilation again to copy it."
    );

    write_record(folder, *totala, 2);
    write_bytes(folder / "rev31.gp3", archive_bytes());
    write_bytes(folder / "totala1.hpi", archive_bytes());
    OA_CHECK(!compose_managed_content(request, values).has_value());
    OA_CHECK(copied_content_bundles(request.user_folder).empty());
    OA_CHECK(
        managed_copy_problem(request, values) ==
        "Open Annihilation's copy of Total Annihilation 3.1c in " + where +
            " is not finished: totala2.hpi, Data/1.zrb, Data/2.zrb, Data/5.zrb were not "
            "copied. Start Open Annihilation again to finish it. If the place you copy from "
            "holds a file at another size, copy from another place."
    );

    ManagedMount emptied{};
    emptied.user_folder = scratch / "emptied-player";
    const fs::path emptied_folder = managed_folder(emptied.user_folder, *totala);
    write_record(emptied_folder, *totala);
    OA_CHECK(!compose_managed_content(emptied, values).has_value());
    OA_CHECK(
        managed_copy_problem(emptied, values) ==
        "Open Annihilation's copy of Total Annihilation 3.1c in " + path_to_utf8(emptied_folder) +
            " holds no archive the game can mount. Delete that folder and start Open "
            "Annihilation again to copy the game afresh."
    );
}

void check_case(const fs::path& scratch) {
    const bundles::Bundle* totala = bundles::find_bundle("totala");
    OA_CHECK(totala != nullptr);
    if (totala == nullptr)
        return;
    const fs::path user = scratch / "case-player";
    const fs::path folder = managed_folder(user, *totala);
    write_record(folder, *totala);
    write_bytes(folder / "TOTALA1.HPI", archive_bytes());
    write_bytes(folder / "DATA" / "1.ZRB", std::vector<uint8_t>{'m'});
    write_bytes(folder / "Music" / "2.mp3", std::vector<uint8_t>{'s'});

    const oa::platform::preferences::Values values;
    const std::optional<GameDirectory> mounted = mount_of(user, "totala", values);
    OA_CHECK(mounted.has_value());
    if (!mounted)
        return;
    OA_CHECK(contains(file_names(mounted->archives), "TOTALA1.HPI"));
    OA_CHECK(roots_hold_movies(mounted->folders));
    const fs::path music = oa::audio::music_disc_directory(mounted->installation);
    OA_CHECK(oa::audio::music_disc_present(oa::audio::music_disc_scan(music)));
    OA_CHECK(copied_content_bundles(user) == std::vector<std::string>{"totala"});
}

/// Reads one file of tests/data/stock-files.
///
/// @param relative its path under that folder
/// @return its bytes
[[nodiscard]] std::vector<uint8_t> fixture_bytes(std::string_view relative) {
    const fs::path path = fs::path(OA_STOCK_FILES_FIXTURE) / path_from_utf8(relative);
    std::ifstream in(path, std::ios::binary);
    OA_CHECK(static_cast<bool>(in));
    return std::vector<uint8_t>(
        std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()
    );
}

/// An archive of fixture files.
///
/// @param files each file's path in the archive, then the fixture it holds
/// @return the archive's bytes
[[nodiscard]] std::vector<uint8_t>
fixture_archive(std::initializer_list<std::pair<std::string_view, std::string_view>> files) {
    std::vector<oa::HpiWriteFile> written;
    for (const auto& [inside, relative] : files)
        written.push_back(oa::HpiWriteFile{std::string(inside), fixture_bytes(relative)});
    return oa::write_hpi(written);
}

/// The key the unit sync knows a unit by: the hash of its whole FBI file.
///
/// @param bytes the FBI file
/// @return its key
[[nodiscard]] uint32_t fbi_key(const std::vector<uint8_t>& bytes) {
    return oa::formats::tdf::buffer_hash(bytes.data(), static_cast<int32_t>(bytes.size()));
}

/// A bundle table that knows the files a test placed: each listed bundle of
/// the built-in table, its files' sizes and SHA-256s read from the copy.
struct KnownTable {
    std::vector<std::vector<bundles::ContentFile>> files;
    std::vector<std::vector<oa::base::sha256::Digest>> digests;
    std::vector<bundles::Bundle> bundles;

    KnownTable(const fs::path& user, std::initializer_list<std::string_view> ids) {
        files.reserve(ids.size());
        digests.reserve(ids.size());
        for (const std::string_view id : ids) {
            const bundles::Bundle* bundle = bundles::find_bundle(id);
            OA_CHECK(bundle != nullptr);
            if (bundle == nullptr)
                continue;
            auto& known_digests = digests.emplace_back();
            known_digests.reserve(bundle->files.size());
            auto& known_files = files.emplace_back();
            for (const bundles::ContentFile& file : bundle->files) {
                const fs::path placed =
                    managed_folder(user, *bundle) / path_from_utf8(file.copy_name);
                std::ifstream in(placed, std::ios::binary);
                const std::vector<uint8_t> bytes{
                    std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()
                };
                known_digests.push_back(oa::base::sha256::digest_of(bytes));
                bundles::ContentFile known = file;
                known.size = bytes.size();
                known.sha256 = std::span<const oa::base::sha256::Digest>(&known_digests.back(), 1);
                known_files.push_back(known);
            }
            bundles::Bundle copy = *bundle;
            copy.files = known_files;
            bundles.push_back(copy);
        }
    }
};

/// The 0.9.0 incident: a stray rev31.gp3 in a
/// managed expansion folder. The composed folders list the expansions
/// before the base game, and discovery merges them by name, the earlier
/// folder's file winning, so the stray took the 3.1 patch archive's place:
/// its ARMMEX.FBI replaced the patch's, and ARMYORK's weapon fell through to
/// Battle Tactics' copy. Only the bundle table's archives are mounted now.
void check_stray_patch_archive(const fs::path& scratch) {
    const fs::path user = scratch / "stray-player";
    const oa::platform::preferences::Values values;
    const std::string_view content = "totala:campaign,core-contingency,battle-tactics";
    fs::path base_folder;
    fs::path tactics_folder;
    for (const std::string_view id : {"totala", "campaign", "core-contingency", "battle-tactics"}) {
        const bundles::Bundle* bundle = bundles::find_bundle(id);
        OA_CHECK(bundle != nullptr);
        if (bundle == nullptr)
            return;
        const fs::path folder = managed_folder(user, *bundle);
        place_bundle(folder, *bundle, archive_bytes());
        write_record(folder, *bundle);
        if (id == "totala")
            base_folder = folder;
        if (id == "battle-tactics")
            tactics_folder = folder;
    }
    write_bytes(
        base_folder / "rev31.gp3",
        fixture_archive(
            {{"units/ARMMEX.FBI", "patch/units/ARMMEX.FBI"},
             {"weapons/ARMYORK_weapon.tdf", "patch/weapons/ARMYORK_weapon.tdf"}}
        )
    );
    write_bytes(
        tactics_folder / "btdata.ccx",
        fixture_archive(
            {{"weapons/ARMYORK_weapon.tdf", "battle-tactics/weapons/ARMYORK_weapon.tdf"}}
        )
    );
    const fs::path stray = tactics_folder / "rev31.gp3";
    write_bytes(
        stray,
        fixture_archive(
            {{"units/ARMMEX.FBI", "stray/units/ARMMEX.FBI"},
             {"units/RGSTRAY.FBI", "stray/units/RGSTRAY.FBI"}}
        )
    );
    const uint32_t stock_key = fbi_key(fixture_bytes("patch/units/ARMMEX.FBI"));
    const uint32_t stray_key = fbi_key(fixture_bytes("stray/units/ARMMEX.FBI"));
    OA_CHECK(stock_key != stray_key);

    const std::optional<GameDirectory> composed = mount_of(user, content, values);
    OA_CHECK(composed.has_value());
    if (!composed)
        return;
    // The search folders, highest first: each expansion that is on, then
    // the base game. Battle Tactics' folder comes before Games/totala.
    OA_CHECK(composed->folders.size() == 4);
    OA_CHECK(!composed->folders.empty() && composed->folders.back() == base_folder);
    OA_CHECK(
        std::find(composed->folders.begin(), composed->folders.end(), tactics_folder) !=
        composed->folders.end()
    );

    // Before: discovery over those folders as 0.9.1 ran it, with no list of
    // the bundles' archives. The stray is mounted first, as the revision
    // archive, and the patch archive of the same name is not mounted at all.
    {
        oa::AssetStore before(composed->folders);
        std::ignore = before.discover(discovery_plan_of(nullptr));
        const auto mounted = before.mount_paths();
        std::error_code failure;
        OA_CHECK(!mounted.empty() && fs::equivalent(mounted.front(), stray, failure));
        bool patch_mounted = false;
        for (const fs::path& archive : mounted)
            patch_mounted =
                patch_mounted || fs::equivalent(archive, base_folder / "rev31.gp3", failure);
        OA_CHECK(!patch_mounted);
        const oa::AssetData unit = before.read("units/ARMMEX.FBI");
        OA_CHECK(fbi_key(unit.bytes) == stray_key);
        OA_CHECK(before.read_if_present("units/RGSTRAY.FBI").has_value());
        const oa::AssetData weapon = before.read("weapons/ARMYORK_weapon.tdf");
        OA_CHECK(fs::equivalent(weapon.source, tactics_folder / "btdata.ccx", failure));
    }

    // After: the composed mount reads only the archives the bundle table
    // lists from each bundle's folder. The patch archive mounts first and
    // serves the stock ARMMEX.FBI and its own ARMYORK weapon.
    std::error_code failure;
    for (const fs::path& archive : composed->archives)
        OA_CHECK(!fs::equivalent(archive, stray, failure));
    OA_CHECK(
        !composed->archives.empty() &&
        fs::equivalent(composed->archives.front(), base_folder / "rev31.gp3", failure)
    );
    oa::AssetStore after(composed->folders);
    for (const fs::path& archive : composed->archives)
        after.mount(archive);
    OA_CHECK(fbi_key(after.read("units/ARMMEX.FBI").bytes) == stock_key);
    OA_CHECK(!after.read_if_present("units/RGSTRAY.FBI").has_value());
    const oa::AssetData weapon = after.read("weapons/ARMYORK_weapon.tdf");
    OA_CHECK(fs::equivalent(weapon.source, base_folder / "rev31.gp3", failure));

    // The content check names the stray, with its folder and size, against a
    // table that knows every other file here: the stray alone differs.
    KnownTable known(user, {"totala", "campaign", "core-contingency", "battle-tactics"});
    const ContentCheck check = check_managed_content(user, known.bundles);
    const ContentBundleCheck* tactics = nullptr;
    uint32_t differ = 0;
    for (const ContentBundleCheck& bundle : check.bundles) {
        differ += bundle.files_differ;
        if (bundle.id == "battle-tactics")
            tactics = &bundle;
    }
    OA_CHECK(tactics != nullptr && differ == 1);
    if (tactics == nullptr)
        return;
    OA_CHECK(
        tactics->files_differ == 1 && tactics->differing.empty() && tactics->strays.size() == 1
    );
    if (tactics->strays.size() != 1)
        return;
    const ContentStrayArchive& found = tactics->strays.front();
    OA_CHECK(found.name == "rev31.gp3" && found.folder == tactics_folder);
    OA_CHECK(found.size == fs::file_size(stray));
    OA_CHECK(content_bundle_line(*tactics) == "rev31.gp3 is not one of its files");
    const std::string named = content_stray_line(*tactics, found);
    OA_CHECK(
        named == "rev31.gp3 in " + path_to_utf8(tactics_folder) + ", " +
                     std::to_string(found.size) +
                     " bytes, is not a file of Battle Tactics. The game does not read it."
    );
    OA_CHECK(content_notice_due(check));
    bool listed = false;
    for (const std::string& line : content_file_notice(check).lines)
        listed = listed || line == named;
    OA_CHECK(listed);

    // The presence record's bundles say the files differ; a stray in a
    // bundle that is not played does not.
    const std::vector<std::string> played{"campaign", "core-contingency", "battle-tactics"};
    OA_CHECK(played_content_differs(check, "totala", played));
    const std::vector<std::string> without_tactics{"campaign", "core-contingency"};
    OA_CHECK(!played_content_differs(check, "totala", without_tactics));

    // The Boneyards entry is refused, naming the file, and Repair removes it.
    const StockFilesRefusal refusal = stock_files_refusal(check, user, "boneyards");
    OA_CHECK(refusal.refused && refusal.stray && refusal.file == "rev31.gp3");
    OA_CHECK(refusal.folder == tactics_folder && refusal.size == found.size);
    OA_CHECK(
        refusal.sentence == "rev31.gp3 in " + path_to_utf8(tactics_folder) +
                                " is not one of Total Annihilation's files, so the Boneyards "
                                "cannot be entered. Remove it to enter."
    );

    // With the stray gone, the copy is Total Annihilation's again.
    fs::remove(stray);
    const ContentCheck repaired = check_managed_content(user, known.bundles);
    OA_CHECK(!content_notice_due(repaired));
    OA_CHECK(!played_content_differs(repaired, "totala", played));
    OA_CHECK(!stock_files_refusal(repaired, user, "boneyards").refused);
}

/// A game folder that is not Open Annihilation's own copy: an archive the
/// bundle table names must be that file, and any other archive is a stray.
void check_unmanaged_folder(const fs::path& scratch) {
    const fs::path folder = scratch / "old-install";
    write_bytes(folder / "rev31.gp3", archive_bytes());
    write_bytes(folder / "readme.txt", std::vector<uint8_t>{'r'});
    // rev31.gp3 at another size than the table's differs; it is not hashed.
    ContentCheck check = check_game_folder(folder);
    OA_CHECK(check.bundles.size() == 1);
    if (check.bundles.size() != 1)
        return;
    OA_CHECK(check.bundles[0].differing.size() == 1 && check.bundles[0].strays.empty());
    OA_CHECK(check.bundles[0].differing[0].copy_name == "rev31.gp3");
    write_bytes(folder / "Cometctr.ufo", archive_bytes());
    check = check_game_folder(folder);
    OA_CHECK(check.bundles[0].strays.size() == 1 && check.bundles[0].files_differ == 2);
    OA_CHECK(!check.bundles[0].strays.empty() && check.bundles[0].strays[0].name == "Cometctr.ufo");
    const StockFilesRefusal refusal = stock_files_refusal(check, scratch, {});
    OA_CHECK(refusal.refused && refusal.stray && refusal.file == "Cometctr.ufo");
    // The first finding is enough for the entry.
    const ContentCheck first = check_game_folder(folder, true);
    OA_CHECK(first.bundles.size() == 1 && first.bundles[0].files_differ == 1);
    // A folder with no archive at all holds nothing that is not known.
    const fs::path empty = scratch / "empty-install";
    fs::create_directories(empty);
    OA_CHECK(!stock_files_refusal(check_game_folder(empty), scratch, {}).refused);
}

} // namespace

int main() {
    const fs::path scratch = oa::test::make_scratch_directory("G05-content-mount");
    try {
        check_composed_mount(scratch);
        check_unfinished_copy(scratch);
        check_case(scratch);
        check_stray_patch_archive(scratch);
        check_unmanaged_folder(scratch);
    } catch (const std::exception& failure) {
        std::fprintf(stderr, "content mount: %s\n", failure.what());
        ++oa::test::failed_checks();
    }
    std::error_code failure;
    fs::remove_all(scratch, failure);
    if (oa::test::failed_checks() == 0)
        std::puts("content mount checks passed");
    return oa::test::check_exit_status();
}
