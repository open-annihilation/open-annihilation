// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A scratch Keys folder listed from its manifests: two installed layouts, an
// own layout, a broken manifest, an oversized one, a staging folder, a link,
// two folders with one id, a missing folder, and an own layout written whole.

#include "oa/data/key_library/library.hpp"
#include "oa/formats/oakeys/manifest.hpp"
#include "oa/test/check.hpp"
#include "oa/test/scratch_directory.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>

namespace {

namespace fs = std::filesystem;
namespace files = oa::formats::oakeys;
namespace lib = oa::data::key_library;

/// A scratch folder, deleted when the test ends.
class Scratch {
  public:

    Scratch() : path_(oa::test::make_scratch_directory("oa-data-key-library-")) {}

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

/// Writes a file's bytes.
///
/// @param file the file
/// @param text its text
void write_text(const fs::path& file, std::string_view text) {
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    OA_CHECK(static_cast<bool>(out));
}

/// A manifest that reads.
///
/// @param id the layout's id
/// @param name the name players see
/// @return the manifest
files::Manifest manifest_of(std::string id, std::string name) {
    files::Manifest manifest;
    manifest.format = files::format_version;
    manifest.id = std::move(id);
    manifest.name = std::move(name);
    manifest.version.major = 1;
    manifest.version.parts = 1;
    manifest.author.name = "Ada";
    manifest.tags = {"grid"};
    manifest.base = files::Base::oa_default;
    return manifest;
}

/// Writes a layout's manifest.
///
/// @param folder the layout's folder
/// @param manifest the manifest
void write_layout(const fs::path& folder, const files::Manifest& manifest) {
    write_text(folder / "oakeys.yaml", files::write_manifest(manifest));
}

/// Tells whether a problem names a phrase.
///
/// @param listing the listing
/// @param phrase the text a problem holds
/// @return true when one problem holds it
bool problem_has(const lib::Listing& listing, std::string_view phrase) {
    for (const std::string& problem : listing.problems)
        if (problem.find(phrase) != std::string::npos)
            return true;
    return false;
}

/// Tells whether a folder holds a temporary file.
///
/// @param folder the folder
/// @return true when a name ends in .tmp
bool has_temporary(const fs::path& folder) {
    std::error_code error;
    if (!fs::exists(folder, error))
        return false;
    for (const fs::directory_entry& entry : fs::directory_iterator(folder)) {
        const std::string name = entry.path().filename().string();
        if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".tmp") == 0)
            return true;
    }
    return false;
}

void test_listing(const Scratch& scratch) {
    const fs::path keys = scratch.path() / "Keys";
    write_layout(keys / "ridge-calm", manifest_of("ridge-calm", "Ridge Calm"));
    write_layout(keys / "ridge-hands", manifest_of("ridge-hands", "Ridge Hands"));
    write_text(
        keys / "ridge-calm" / ".oa-origin.yaml",
        "oa-origin: 1\n"
        "origin: catalogue\n"
        "registry: ridge\n"
        "catalogue-id: ridge-calm\n"
        "release: 7\n"
        "sha256: \"abc123\"\n"
        "installed: \"2000-01-01\"\n"
    );
    write_layout(keys / "own-oa-default", manifest_of("own-oa-default", "Your changes"));
    write_text(
        keys / "own-oa-default" / ".oa-origin.yaml",
        "oa-origin: 1\n"
        "origin: own\n"
    );
    write_text(
        keys / "broken-one" / "oakeys.yaml",
        "oakeys: 1\n"
        "id: \"broken-one\"\n"
        "name: \"Broken\"\n"
        "version: \"1\"\n"
        "author:\n"
        "  name: \"Ada\"\n"
    );
    write_text(keys / "huge-yaml" / "oakeys.yaml", std::string((256 * 1024) + 1, 'y'));
    fs::create_directories(keys / ".staging");
    write_text(
        keys / ".staging" / "oakeys.yaml", files::write_manifest(manifest_of("staged", "Staged"))
    );
    write_layout(keys / "hands-copy", manifest_of("ridge-hands", "Ridge Hands"));
    const fs::path elsewhere = scratch.path() / "elsewhere";
    write_layout(elsewhere, manifest_of("linked-away", "Linked Away"));
    std::error_code link_error;
    fs::create_directory_symlink(elsewhere, keys / "linked-hands", link_error);

    const lib::Listing listing = lib::list_installed(keys);
    OA_CHECK(lib::find(listing, "ridge-calm") != nullptr);
    OA_CHECK(lib::find(listing, "ridge-hands") != nullptr);
    OA_CHECK(lib::find(listing, "own-oa-default") != nullptr);
    OA_CHECK(lib::find(listing, "staged") == nullptr);
    OA_CHECK(lib::find(listing, "broken-one") == nullptr);
    OA_CHECK(lib::find(listing, "linked-away") == nullptr);
    OA_CHECK(lib::find(listing, "no-such") == nullptr);
    OA_CHECK(listing.layouts.size() == 3);

    const lib::Installed* calm = lib::find(listing, "ridge-calm");
    OA_CHECK(calm != nullptr && calm->origin == lib::Origin::catalogue);
    OA_CHECK(calm != nullptr && calm->manifest.name == "Ridge Calm");
    const lib::Installed* hands = lib::find(listing, "ridge-hands");
    OA_CHECK(hands != nullptr && hands->origin == lib::Origin::file);
    OA_CHECK(hands != nullptr && hands->folder.filename() == "ridge-hands");
    const lib::Installed* own = lib::find(listing, "own-oa-default");
    OA_CHECK(own != nullptr && own->origin == lib::Origin::own);

    OA_CHECK(problem_has(listing, "broken-one: oakeys.yaml:"));
    OA_CHECK(problem_has(listing, "huge-yaml: oakeys.yaml: it is larger than 256 KiB"));
    OA_CHECK(problem_has(listing, "hands-copy: it has the same id ridge-hands as ridge-hands"));
    OA_CHECK(!problem_has(listing, "staged"));
    if (!link_error)
        OA_CHECK(!problem_has(listing, "linked-away") && !problem_has(listing, "linked-hands"));

    const lib::Listing missing = lib::list_installed(scratch.path() / "no-keys");
    OA_CHECK(missing.layouts.empty());
    OA_CHECK(missing.problems.empty());
}

void test_write_own(const Scratch& scratch) {
    const fs::path keys = scratch.path() / "Keys-write";
    const fs::path own = keys / "own-oa-default";
    write_text(own / "oakeys.yaml", std::string(80, 'x') + "\nnot whole\n");
    files::Manifest manifest = manifest_of("own-oa-default", "Your changes");
    manifest.version.major = 1;
    manifest.version.minor = 0;
    manifest.version.parts = 2;
    manifest.author.name = "You";
    manifest.tags = {"own"};
    files::BindingEntry binding;
    binding.action = "build.line";
    binding.chords.push_back(files::ChordText{"@KeyQ", 0, 0});
    manifest.bindings.push_back(std::move(binding));
    std::string error;
    OA_CHECK(lib::write_own(keys, "oa-default", manifest, error));
    OA_CHECK(error.empty());
    OA_CHECK(lib::own_folder_name("oa-default") == "own-oa-default");
    {
        std::ifstream in(own / "oakeys.yaml", std::ios::binary);
        const std::string kept{std::istreambuf_iterator<char>{in}, {}};
        OA_CHECK(kept == files::write_manifest(manifest));
        OA_CHECK(kept.find("@KeyQ") != std::string::npos);
        OA_CHECK(kept.find("not whole") == std::string::npos);
    }
    {
        std::ifstream in(own / ".oa-origin.yaml", std::ios::binary);
        const std::string origin{std::istreambuf_iterator<char>{in}, {}};
        OA_CHECK(origin.find("origin: own") != std::string::npos);
    }
    OA_CHECK(!has_temporary(own));
    OA_CHECK(!has_temporary(keys));

    OA_CHECK(!lib::write_own(keys, "..", manifest, error));
    OA_CHECK(!error.empty());
    OA_CHECK(!lib::write_own(keys, "", manifest, error));
    OA_CHECK(!lib::write_own(keys, "a/b", manifest, error));
    OA_CHECK(!lib::write_own(keys, "a\\b", manifest, error));
    std::error_code missing;
    OA_CHECK(!fs::exists(keys / "own-..", missing));
    OA_CHECK(!fs::exists(keys / "own-a", missing));
    OA_CHECK(!fs::exists(scratch.path() / "b", missing));
}

} // namespace

int main() {
    const Scratch scratch;
    test_listing(scratch);
    test_write_own(scratch);
    return oa::test::check_exit_status();
}
