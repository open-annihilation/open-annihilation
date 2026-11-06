// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Installing an expansion. The fixture package is refused by the built-in
// table, and nothing is written. The same package, checked against the test
// table, lands in expansions/<game>/<id> with the .oa-content record, which
// names the package. A second install keeps a file the table does not
// name. Uninstall removes that folder and leaves a sibling. A link in the
// expansion's place is left where it is. A package that also holds a stray
// file writes nothing. A package shaped as an archive beside a folder is
// installed again over its copy file by file: the folder stays, a war map
// the game wrote in it is kept, a changed file is put right, a file only the
// old record named goes, a file the old record names outside the folder (by
// "..", or by an absolute path) is left, an empty numbered copy of the
// folder goes, and the record names the new package.

#include "oa/app/package_install.hpp"
#include "oa/app/package_install/oaexp.hpp"
#include "oa/base/sha256.hpp"
#include "oa/data/content_bundles/expansion_fixture.hpp"
#include "oa/data/content_bundles/record.hpp"
#include "oa/formats/zip.hpp"
#include "oa/test/check.hpp"
#include "oa/test/scratch_directory.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace bundles = oa::data::content_bundles;
namespace fixture = oa::test::expansion_fixture;
namespace install = oa::app::package_install;
namespace zip = oa::formats::zip;
using install::Refusal;

class Scratch {
  public:

    Scratch() : path_(oa::test::make_scratch_directory("oa-package-oaexp-")) {}

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

void write_text(const fs::path& file, std::string_view text) {
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string read_text(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

bool write_zip(const fs::path& file, std::span<const zip::NewEntry> entries) {
    std::vector<uint8_t> archive;
    zip::ZipError error;
    if (!zip::write_archive(entries, archive, error))
        return false;
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(
        reinterpret_cast<const char*>(archive.data()), static_cast<std::streamsize>(archive.size())
    );
    return static_cast<bool>(out);
}

bool make_link(const fs::path& to, const fs::path& made_link) {
    std::error_code error;
    fs::create_directory_symlink(to, made_link, error);
    if (!error && !install::is_link_or_junction(made_link))
        error = std::make_error_code(std::errc::no_such_file_or_directory);
    if (error)
        std::fprintf(stderr, "skipped: this system made no link: %s\n", error.message().c_str());
    return !error;
}

void test_install() {
    Scratch scratch;
    const fs::path package = scratch.path() / "cavedog-units.oaexp";
    const zip::NewEntry entries[] = {
        {"oaexp.yaml", fixture::manifest_bytes()},
        {"unit.fbi", fixture::unit_bytes()},
    };
    OA_CHECK(write_zip(package, entries));

    static constexpr uint8_t extra_byte[] = {'x'};
    const fs::path stray_package = scratch.path() / "stray.oaexp";
    const zip::NewEntry stray_entries[] = {
        {"oaexp.yaml", fixture::manifest_bytes()},
        {"unit.fbi", fixture::unit_bytes()},
        {"extra.txt", std::span<const uint8_t>(extra_byte, 1)},
    };
    OA_CHECK(write_zip(stray_package, stray_entries));

    const fs::path player = scratch.path() / "player";
    fs::create_directories(player);
    install::Problem problem;
    fs::path installed;
    OA_CHECK(!install::oaexp::install_oaexp(package, player, "core-prime", &installed, problem));
    OA_CHECK(fs::is_empty(player));

    OA_CHECK(!install::oaexp::install_expansion_package(
        stray_package, player, "core-prime", fixture::test_bundle(), &installed, problem
    ));
    OA_CHECK(problem.refusal == Refusal::stray_file);
    OA_CHECK(fs::is_empty(player));

    OA_CHECK(
        install::oaexp::install_expansion_package(
            package, player, "core-prime", fixture::test_bundle(), &installed, problem
        )
    );
    const fs::path folder = installed;
    OA_CHECK(folder.filename() == "cavedog-units");
    OA_CHECK(folder.parent_path().filename() == "totala");
    // The managed expansions folder, the name the installer uses.
    OA_CHECK(folder.parent_path().parent_path().filename() == bundles::managed_expansions_folder);
    OA_CHECK(folder.parent_path().parent_path().parent_path() == player);
    OA_CHECK(read_text(folder / "unit.fbi") == "abcd");
    OA_CHECK(!fs::exists(folder / "oaexp.yaml"));
    const bundles::ContentRecord record = bundles::read_bundle_folder_record(folder, nullptr);
    OA_CHECK(record.valid);
    OA_CHECK(record.bundle_id == "cavedog-units");
    OA_CHECK(record.source == "registry core-prime");
    OA_CHECK(record.files.size() == 1);
    if (!record.files.empty()) {
        OA_CHECK(record.files[0].copy_name == "unit.fbi");
        OA_CHECK(record.files[0].size == 4);
    }

    // A file the player opened records its own SHA-256 and no release.
    OA_CHECK(record.package.has_value());
    if (record.package) {
        std::ifstream in(package, std::ios::binary);
        const std::vector<uint8_t> bytes{
            std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}
        };
        OA_CHECK(record.package->sha256 == oa::base::sha256::digest_of(bytes));
        OA_CHECK(record.package->release == 0);
    }

    write_text(folder / "junk.txt", "junk");
    OA_CHECK(
        install::oaexp::install_expansion_package(
            package, player, "core-prime", fixture::test_bundle(), &installed, problem
        )
    );
    OA_CHECK(read_text(folder / "junk.txt") == "junk");
    fs::remove(folder / "junk.txt");
    OA_CHECK(read_text(folder / "unit.fbi") == "abcd");
    OA_CHECK(!fs::exists(folder.parent_path() / ".oaexp-staging-cavedog-units"));
    OA_CHECK(!fs::exists(folder.parent_path() / ".oaexp-old-cavedog-units"));

    const fs::path sibling = folder.parent_path() / "music";
    write_text(sibling / "keep.txt", "keep");
    OA_CHECK(install::oaexp::uninstall_expansion(player, "cavedog-units", problem));
    OA_CHECK(!fs::exists(folder));
    OA_CHECK(read_text(sibling / "keep.txt") == "keep");

    const fs::path linked_player = scratch.path() / "linked";
    const fs::path outside = scratch.path() / "outside";
    write_text(outside / "marker.txt", "keep");
    const fs::path link = linked_player / fs::relative(folder, player);
    fs::create_directories(link.parent_path());
    if (!make_link(outside, link))
        return;
    OA_CHECK(!install::oaexp::install_expansion_package(
        package, linked_player, "core-prime", fixture::test_bundle(), &installed, problem
    ));
    OA_CHECK(problem.refusal == Refusal::link);
    OA_CHECK(install::is_link_or_junction(link));
    OA_CHECK(read_text(outside / "marker.txt") == "keep");
    OA_CHECK(!install::oaexp::uninstall_expansion(linked_player, "cavedog-units", problem));
    OA_CHECK(problem.refusal == Refusal::link);
    OA_CHECK(install::is_link_or_junction(link));
    OA_CHECK(read_text(outside / "marker.txt") == "keep");
}

/// The names of a folder's entries, sorted.
std::vector<std::string> names_in(const fs::path& folder) {
    std::vector<std::string> names;
    std::error_code error;
    for (fs::directory_iterator cursor(folder, error), end; !error && cursor != end;
         cursor.increment(error))
        names.push_back(cursor->path().filename().generic_string());
    std::sort(names.begin(), names.end());
    return names;
}

void test_reinstall_in_place() {
    Scratch scratch;
    const fs::path package = scratch.path() / "folder.oaexp";
    const zip::NewEntry entries[] = {
        {"oaexp.yaml", fixture::manifest_bytes()},
        {"unit.fbi", fixture::unit_bytes()},
        {"Units/arm.fbi", fixture::folder_bytes()},
    };
    OA_CHECK(write_zip(package, entries));
    const fs::path player = scratch.path() / "player";
    fs::create_directories(player);
    const auto first = oa::base::sha256::digest_of(fixture::unit_bytes());
    const auto second = oa::base::sha256::digest_of(fixture::folder_bytes());
    install::Problem problem;
    fs::path folder;
    install::oaexp::PackageFrom release_one;
    release_one.sha256 = first;
    release_one.release = 1;
    OA_CHECK(
        install::oaexp::install_expansion_package(
            package, player, "core-prime", fixture::folder_bundle(), &folder, problem, release_one
        )
    );
    bundles::ContentRecord record = bundles::read_bundle_folder_record(folder, nullptr);
    OA_CHECK(record.valid && record.package.has_value());
    if (record.package)
        OA_CHECK(record.package->sha256 == first && record.package->release == 1);
    OA_CHECK(read_text(folder / "Units" / "arm.fbi") == "efgh");

    // What the game wrote beside the package's own files, a file only the
    // old record names, a damaged file, and the empty and the full numbered
    // copies a file sync leaves of the folder.
    write_text(folder / "Units" / "area_map.gwm", "war map");
    write_text(folder / "notes.txt", "kept");
    write_text(folder / "unit.fbi", "zzzz");
    write_text(folder / "old.fbi", "old");
    bundles::ContentRecordFile old_item;
    old_item.copy_name = "old.fbi";
    old_item.size = 3;
    record.files.push_back(old_item);
    // Names that leave the expansion's folder, which a damaged or edited
    // record may hold. The files they name are not the package's.
    const fs::path beside = folder.parent_path() / "outside.txt";
    const fs::path elsewhere = scratch.path() / "absolute.txt";
    write_text(beside, "beside");
    write_text(elsewhere, "elsewhere");
    for (const std::string& name :
         {std::string("../outside.txt"), elsewhere.generic_string(), std::string("./old.fbi")}) {
        bundles::ContentRecordFile outside_item;
        outside_item.copy_name = name;
        outside_item.size = 1;
        record.files.push_back(outside_item);
    }
    const std::optional<std::string> old_text = bundles::format_content_record(record);
    OA_CHECK(old_text.has_value());
    if (old_text)
        write_text(folder / std::string(bundles::content_record_name), *old_text);
    fs::create_directories(folder / "Units 2" / "Empty");
    write_text(folder / "Units 3" / "copy.txt", "kept");

    install::oaexp::PackageFrom release_two;
    release_two.sha256 = second;
    release_two.release = 2;
    fs::path again;
    OA_CHECK(
        install::oaexp::install_expansion_package(
            package, player, "core-prime", fixture::folder_bundle(), &again, problem, release_two
        )
    );
    OA_CHECK(again == folder);
    OA_CHECK(read_text(folder / "Units" / "area_map.gwm") == "war map");
    OA_CHECK(read_text(folder / "notes.txt") == "kept");
    OA_CHECK(read_text(folder / "unit.fbi") == "abcd");
    OA_CHECK(read_text(folder / "Units" / "arm.fbi") == "efgh");
    OA_CHECK(!fs::exists(folder / "old.fbi"));
    OA_CHECK(read_text(beside) == "beside");
    OA_CHECK(read_text(elsewhere) == "elsewhere");
    OA_CHECK(!fs::exists(folder / "Units 2"));
    OA_CHECK(read_text(folder / "Units 3" / "copy.txt") == "kept");
    // No staging folder and no folder put aside is left.
    OA_CHECK(
        (names_in(folder.parent_path()) == std::vector<std::string>{"cavedog-units", "outside.txt"})
    );
    const bundles::ContentRecord now = bundles::read_bundle_folder_record(folder, nullptr);
    OA_CHECK(now.valid && now.package.has_value() && now.files.size() == 2);
    if (now.package)
        OA_CHECK(now.package->sha256 == second && now.package->release == 2);
}

} // namespace

int main() {
    test_install();
    test_reinstall_in_place();
    return oa::test::check_exit_status();
}
