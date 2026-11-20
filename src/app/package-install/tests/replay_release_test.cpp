// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A replay's release, kept beside the one in use. A folder that does not
// verify is replaced. A folder whose origin names the same digest is left
// as it is, and is then found by that digest.

#include "oa/app/package_install/origin.hpp"
#include "oa/app/package_install/replay_release.hpp"
#include "oa/base/sha256.hpp"
#include "oa/test/check.hpp"
#include "oa/test/raw_zip.hpp"
#include "oa/test/scratch_directory.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace install = oa::app::package_install;
namespace raw = oa::test::raw_zip;
namespace sha = oa::base::sha256;

/// A profile of the made-up mod the package holds.
///
/// @return its text
std::string profile_text() {
    return "oamod: 1\n"
           "id: example-mod\n"
           "name: Example Mod\n"
           "version: \"1.0\"\n"
           "description: A made-up mod the tests install.\n"
           "requires: {base: ta-3.1c, catalogue: 1}\n"
           "author: {name: unknown}\n"
           "packaging: {revision: 1, date: 2026-10-04, packager: Open Annihilation}\n";
}

/// Writes a package of that mod.
///
/// @param folder where it goes
/// @return the package
fs::path write_package(const fs::path& folder) {
    const fs::path file = folder / "example-mod-1.0-1.oamod";
    const auto archive = raw::build_raw_archive({
        raw::stored_text("oamod.yaml", profile_text()),
        raw::stored_text("units/", ""),
        raw::stored_text("units/unit.fbi", "revision 1"),
    });
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(
        reinterpret_cast<const char*>(archive.bytes.data()),
        static_cast<std::streamsize>(archive.bytes.size())
    );
    return file;
}

/// Returns the SHA-256 of a file.
///
/// @param file the file
/// @return its digest
sha::Digest file_hash(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    const std::vector<uint8_t> bytes{
        std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}
    };
    return sha::digest_of(bytes);
}

/// Writes text, making the parent folder first.
///
/// @param file the file
/// @param text what it holds
void write_text(const fs::path& file, std::string_view text) {
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
}

/// Reports whether a path is there.
///
/// @param path the path
/// @return true when it is
bool path_exists(const fs::path& path) {
    std::error_code error;
    return fs::exists(path, error) && !error;
}

/// Installs over a folder that does not verify, then leaves a good one.
///
/// @param scratch the player's folder
void test_replaces_a_folder_that_does_not_verify(const fs::path& scratch) {
    const fs::path package = write_package(scratch);
    const sha::Digest digest = file_hash(package);
    const auto hex = sha::to_hex(digest);
    const std::string name(hex.data(), hex.size());
    const fs::path folder = scratch / std::string(install::replay_releases_folder_name) / name;
    write_text(folder / "half.txt", "a half copy");
    install::Origin wrong;
    wrong.sha256 = sha::digest_of(std::vector<uint8_t>{1, 2, 3});
    wrong.installed = "2026-11-01";
    write_text(folder / std::string(install::origin_file_name), install::origin_text(wrong));
    OA_CHECK(!install::replay_release_folder(scratch, name));

    install::Origin origin;
    origin.kind = install::OriginKind::catalogue;
    origin.registry = "ridge-replays";
    origin.catalogue_id = "example-mod";
    origin.release = 1;
    origin.sha256 = digest;
    std::string error;
    const std::optional<fs::path> installed =
        install::install_replay_release(scratch, package, origin, &error);
    OA_CHECK(installed.has_value());
    OA_CHECK(error.empty());
    const std::optional<fs::path> found = install::replay_release_folder(scratch, name);
    OA_CHECK(found.has_value());
    if (installed && found)
        OA_CHECK(*installed == *found);
    OA_CHECK(path_exists(folder / "oamod.yaml"));
    OA_CHECK(path_exists(folder / "units" / "unit.fbi"));
    OA_CHECK(!path_exists(folder / "half.txt"));
    OA_CHECK(!path_exists(folder.parent_path() / (name + ".old")));
    OA_CHECK(!path_exists(folder.parent_path() / (name + ".staging")));

    write_text(folder / "marker.txt", "left as it is");
    error.clear();
    const std::optional<fs::path> again =
        install::install_replay_release(scratch, package, origin, &error);
    OA_CHECK(again.has_value());
    OA_CHECK(error.empty());
    OA_CHECK(path_exists(folder / "marker.txt"));
    OA_CHECK(install::replay_release_folder(scratch, name).has_value());

    // The origin still names the digest, and the package no longer reads.
    // The next install replaces the folder.
    std::error_code removed;
    fs::remove(folder / "oamod.yaml", removed);
    OA_CHECK(!removed);
    error.clear();
    const std::optional<fs::path> replaced =
        install::install_replay_release(scratch, package, origin, &error);
    OA_CHECK(replaced.has_value());
    OA_CHECK(error.empty());
    OA_CHECK(path_exists(folder / "oamod.yaml"));
    OA_CHECK(!path_exists(folder / "marker.txt"));
}

} // namespace

int main() {
    const fs::path scratch = oa::test::make_scratch_directory("oa-package-replay-release-");
    test_replaces_a_folder_that_does_not_verify(scratch);
    std::error_code ignored;
    fs::remove_all(scratch, ignored);
    return oa::test::check_exit_status();
}
