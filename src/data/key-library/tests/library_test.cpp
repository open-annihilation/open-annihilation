// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A scratch Keys folder listed from its manifests: two installed layouts, an
// own layout, a broken manifest, an oversized one, a staging folder, a link,
// two folders with one id, a missing folder, and an own layout written whole.
// Catalogue facts from a catalogue record, a file record and a damaged one.
// The packer writes the same bytes twice, and that package plans an install.

#include "oa/app/package_install.hpp"
#include "oa/app/package_install/oakeys.hpp"
#include "oa/data/key_library/library.hpp"
#include "oa/formats/oakeys/manifest.hpp"
#include "oa/formats/zip/stream.hpp"
#include "oa/test/check.hpp"
#include "oa/test/scratch_directory.hpp"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace files = oa::formats::oakeys;
namespace install = oa::app::package_install;
namespace lib = oa::data::key_library;
namespace zip = oa::formats::zip;

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
    write_layout(keys / "file-one", manifest_of("file-one", "File One"));
    write_text(
        keys / "file-one" / ".oa-origin.yaml",
        "oa-origin: 1\n"
        "origin: file\n"
        "registry: ridge\n"
        "catalogue-id: file-one\n"
        "release: 4\n"
        "sha256: \"kept-sha\"\n"
        "installed: \"2000-01-01\"\n"
    );
    write_layout(keys / "damaged-one", manifest_of("damaged-one", "Damaged"));
    write_text(keys / "damaged-one" / ".oa-origin.yaml", "not: yaml: ::\n");
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
    OA_CHECK(listing.layouts.size() == 5);

    const lib::Installed* calm = lib::find(listing, "ridge-calm");
    OA_CHECK(calm != nullptr && calm->origin == lib::Origin::catalogue);
    OA_CHECK(calm != nullptr && calm->manifest.name == "Ridge Calm");
    OA_CHECK(calm != nullptr && calm->package_sha256 == "abc123");
    OA_CHECK(calm != nullptr && calm->registry == "ridge");
    OA_CHECK(calm != nullptr && calm->catalogue_id == "ridge-calm");
    OA_CHECK(calm != nullptr && calm->release == 7);
    const lib::Installed* hands = lib::find(listing, "ridge-hands");
    OA_CHECK(hands != nullptr && hands->origin == lib::Origin::file);
    OA_CHECK(hands != nullptr && hands->folder.filename() == "ridge-hands");
    const lib::Installed* own = lib::find(listing, "own-oa-default");
    OA_CHECK(own != nullptr && own->origin == lib::Origin::own);
    const lib::Installed* filed = lib::find(listing, "file-one");
    OA_CHECK(filed != nullptr && filed->origin == lib::Origin::file);
    OA_CHECK(filed != nullptr && filed->package_sha256 == "kept-sha");
    OA_CHECK(filed != nullptr && filed->registry.empty());
    OA_CHECK(filed != nullptr && filed->catalogue_id.empty());
    OA_CHECK(filed != nullptr && filed->release == 0);
    const lib::Installed* damaged = lib::find(listing, "damaged-one");
    OA_CHECK(damaged != nullptr && damaged->origin == lib::Origin::file);
    OA_CHECK(damaged != nullptr && damaged->package_sha256.empty());
    OA_CHECK(damaged != nullptr && damaged->registry.empty());
    OA_CHECK(damaged != nullptr && damaged->release == 0);

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

/// Reads archive bytes for a streamed directory.
///
/// @param context the archive (`vector<uint8_t>`)
/// @param offset where the read starts
/// @param bytes where the bytes go
/// @return true when every byte was copied
bool read_memory(void* context, uint64_t offset, std::span<uint8_t> bytes) {
    const auto* archive = static_cast<const std::vector<uint8_t>*>(context);
    if (archive == nullptr || offset > archive->size() || bytes.size() > archive->size() - offset)
        return false;
    std::memcpy(bytes.data(), archive->data() + static_cast<std::size_t>(offset), bytes.size());
    return true;
}

/// The entry of a name, or null.
///
/// @param directory the archive
/// @param name the entry
/// @return the entry
const zip::StreamEntry* entry_named(const zip::StreamDirectory& directory, std::string_view name) {
    for (const zip::StreamEntry& entry : directory.entries) {
        if (entry.name == name)
            return &entry;
    }
    return nullptr;
}

void test_package(const Scratch& scratch) {
    files::Manifest manifest = manifest_of("grid-test", "Grid test");
    manifest.version.parts = 2;
    manifest.icon = "icon.png";
    files::BindingEntry binding;
    binding.action = "order.attack";
    binding.chords.push_back(files::ChordText{"A", 0, 0});
    manifest.bindings.push_back(std::move(binding));
    const std::string text = files::write_manifest(manifest);
    const std::vector<lib::PackageFile> files = {
        {"notes.txt", {'n', 'o', 't', 'e', '\n'}},
        {"oakeys.yaml", {text.begin(), text.end()}},
        {"icon.png", {1, 2, 3}},
    };
    std::string problem;
    const auto first = lib::package_bytes(files, &problem);
    const auto second = lib::package_bytes(files, &problem);
    OA_CHECK(first.has_value() && second.has_value() && *first == *second);
    OA_CHECK(problem.empty());
    if (!first)
        return;
    std::vector<uint8_t> archive = *first;
    zip::SourceHooks source;
    source.context = &archive;
    source.read_at = &read_memory;
    zip::StreamDirectory directory;
    zip::ZipError error{};
    OA_CHECK(
        zip::read_stream_directory(source, first->size(), zip::StreamLimits{}, directory, error)
    );
    OA_CHECK(!directory.entries.empty() && directory.entries.front().name == "oakeys.yaml");
    const zip::StreamEntry* icon = entry_named(directory, "icon.png");
    const zip::StreamEntry* notes = entry_named(directory, "notes.txt");
    OA_CHECK(icon != nullptr && icon->method == zip::Method::stored);
    OA_CHECK(notes != nullptr && notes->method == zip::Method::deflated);

    std::vector<lib::PackageFile> extra = files;
    extra.push_back({"extra.lua", {'x'}});
    OA_CHECK(!lib::package_bytes(extra, &problem));
    OA_CHECK(problem.find("extra.lua") != std::string::npos);

    files::Manifest plain = manifest_of("grid-test", "Grid test");
    plain.version.parts = 2;
    files::BindingEntry attack;
    attack.action = "order.attack";
    attack.chords.push_back(files::ChordText{"A", 0, 0});
    plain.bindings.push_back(std::move(attack));
    const std::string plain_text = files::write_manifest(plain);
    const std::vector<lib::PackageFile> openable = {
        {"README.md", {'#', ' ', 'G', 'r', 'i', 'd', '\n'}},
        {"oakeys.yaml", {plain_text.begin(), plain_text.end()}},
    };
    const auto packed = lib::package_bytes(openable, &problem);
    OA_CHECK(packed.has_value());
    if (!packed)
        return;
    const fs::path out = scratch.path() / "grid-test.oakeys";
    OA_CHECK(lib::write_package(out, *packed, &problem));
    const install::PackageResult opened = install::open_package(out);
    OA_CHECK(opened.package.has_value());
    if (!opened.package)
        return;
    install::Incoming incoming = opened.package->incoming;
    incoming.origin.installed = "2000-01-01";
    const install::PackageKind* kind = install::find_kind("oakeys");
    OA_CHECK(kind != nullptr);
    if (kind == nullptr)
        return;
    const install::InstallPlan plan = install::oakeys::plan_install(
        incoming, install::folder_hooks(*kind, scratch.path() / "pack-keys")
    );
    OA_CHECK(plan.kind == install::PlanKind::ask_install);
}

} // namespace

int main() {
    const Scratch scratch;
    test_listing(scratch);
    test_write_own(scratch);
    test_package(scratch);
    return oa::test::check_exit_status();
}
