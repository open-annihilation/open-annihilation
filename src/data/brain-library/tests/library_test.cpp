// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A scratch Brains folder listed from its manifests: good brains, a broken
// manifest, a folder with none, an oversized manifest, the installer's own
// folder, a .backup holding another id, a symbolic link, two folders with
// one id, a plain file left untouched, catalogue and file origin records, a
// damaged record, an oversized record, network sizes, and a missing folder.

#include "oa/data/brain_library/library.hpp"
#include "oa/formats/oabrain/manifest.hpp"
#include "oa/test/check.hpp"
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
namespace lib = oa::data::brain_library;
namespace brain = oa::formats::oabrain;

/// A scratch folder, deleted when the test ends.
class Scratch {
  public:

    Scratch() : path_(oa::test::make_scratch_directory("P02-oa-data-brain-library-")) {}

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
/// @param id the brain's id
/// @param name the name players see
/// @return the manifest
brain::Manifest manifest_of(std::string id, std::string name) {
    brain::Manifest manifest;
    manifest.format = brain::format_version;
    manifest.id = std::move(id);
    manifest.name = std::move(name);
    manifest.version.major = 1;
    manifest.version.parts = 1;
    manifest.author.name = "Ada";
    manifest.description = "Holds the line.";
    manifest.tags = {"classic"};
    manifest.works_with.push_back(oa::formats::oamut::Condition{"*", {}, {}});
    brain::Level level;
    level.id = "steady";
    level.name = "Steady";
    level.difficulty = brain::Difficulty::easy;
    manifest.levels.push_back(std::move(level));
    return manifest;
}

/// Names a network on a manifest. The file is not written here.
///
/// @param[in,out] manifest the manifest
/// @param id the network's id
/// @param file the network's file
/// @param sha the SHA-256, 64 lower-case hex digits
void add_network(brain::Manifest& manifest, std::string id, std::string file, char sha) {
    brain::Network network;
    network.id = std::move(id);
    network.file = std::move(file);
    network.sha256.assign(64, sha);
    manifest.networks.push_back(std::move(network));
}

/// Writes a brain's manifest, and a script the listing must not need.
///
/// @param folder the brain's folder
/// @param manifest the manifest
void write_brain(const fs::path& folder, const brain::Manifest& manifest) {
    write_text(folder / "oabrain.yaml", brain::write_manifest(manifest));
    write_text(folder / "brain.lua", "return {}\n");
}

/// Tells whether a problem names a folder.
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

void test_listing(const Scratch& scratch) {
    const fs::path brains = scratch.path() / "Brains";
    brain::Manifest calm = manifest_of("calm-line", "Calm Line");
    add_network(calm, "one", "nets/one.oann", 'a');
    add_network(calm, "two", "nets/two.oann", 'b');
    write_brain(brains / "calm-line", calm);
    write_text(brains / "calm-line" / "nets" / "one.oann", std::string(100, 'x'));
    write_text(brains / "calm-line" / "nets" / "two.oann", std::string(250, 'y'));

    brain::Manifest lantern = manifest_of("lantern-scout", "Lantern Scout");
    add_network(lantern, "scout", "nets/scout.oann", 'd');
    write_brain(brains / "lantern-scout", lantern);
    write_text(brains / "lantern-scout" / "nets" / "scout.oann", std::string(12345, 'z'));

    write_brain(brains / "steady-commander", manifest_of("steady-commander", "Steady Commander"));
    brain::Manifest wide = manifest_of("wide-watch", "Wide Watch");
    add_network(wide, "gone", "nets/gone.oann", 'c');
    write_brain(brains / "wide-watch", wide);
    write_brain(brains / "quiet-hold", manifest_of("quiet-hold", "Quiet Hold"));
    write_text(
        brains / "calm-line" / ".oa-origin.yaml",
        "oa-origin: 1\n"
        "origin: catalogue\n"
        "registry: ridge\n"
        "catalogue-id: calm-line\n"
        "release: 7\n"
        "sha256: \"abc123\"\n"
        "installed: \"2000-01-01\"\n"
    );
    // A file origin may name catalogue keys. Only the SHA-256 is kept.
    write_text(
        brains / "steady-commander" / ".oa-origin.yaml",
        "oa-origin: 1\n"
        "origin: file\n"
        "registry: ridge\n"
        "catalogue-id: steady-commander\n"
        "release: 4\n"
        "sha256: \"def456\"\n"
        "installed: \"2000-01-01\"\n"
    );
    write_text(
        brains / "wide-watch" / ".oa-origin.yaml",
        "oa-origin: 1\n"
        "origin: file\n"
        "sha256: \"eee789\"\n"
        "extra: 1\n"
        "installed: \"2000-01-01\"\n"
    );
    write_text(brains / "quiet-hold" / ".oa-origin.yaml", std::string(4097, 'x'));
    write_text(
        brains / "broken-one" / "oabrain.yaml",
        "oabrain: 1\n"
        "id: \"broken-one\"\n"
        "name: \"Broken\"\n"
        "version: \"1\"\n"
        "author:\n"
        "  name: \"Ada\"\n"
        "description: \"Broken.\"\n"
        "works-with: [\"*\"]\n"
        "levels:\n"
        "  - id: only\n"
        "    name: \"Only\"\n"
        "    difficulty: medium\n"
    );
    fs::create_directories(brains / "empty-one");
    write_text(brains / "huge-yaml" / "oabrain.yaml", std::string((256 * 1024) + 1, 'y'));
    write_brain(brains / "hands-copy", manifest_of("steady-commander", "Steady Commander"));
    write_brain(brains / "alpha-copy", manifest_of("other-id", "Other"));
    write_brain(brains / "beta-copy", manifest_of("other-id", "Other"));
    fs::create_directories(brains / ".oabrain-staging-x");
    write_text(
        brains / ".oabrain-staging-x" / "oabrain.yaml",
        brain::write_manifest(manifest_of("staged", "Staged"))
    );
    write_text(
        brains / "steady-commander" / ".backup" / "oabrain.yaml",
        brain::write_manifest(manifest_of("hidden-backup", "Hidden"))
    );
    const fs::path notes = brains / "notes.txt";
    const std::string note = "the player's own notes\n";
    write_text(notes, note);
    const fs::path elsewhere = scratch.path() / "elsewhere";
    write_brain(elsewhere, manifest_of("linked-away", "Linked Away"));
    std::error_code link_error;
    fs::create_directory_symlink(elsewhere, brains / "linked-hands", link_error);

    const lib::Listing listing = lib::list_installed(brains);
    OA_CHECK(listing.brains.size() == 6);
    if (listing.brains.size() == 6) {
        OA_CHECK(listing.brains[0].manifest.id == "calm-line");
        OA_CHECK(listing.brains[1].manifest.id == "lantern-scout");
        OA_CHECK(listing.brains[2].manifest.id == "other-id");
        OA_CHECK(listing.brains[3].manifest.id == "quiet-hold");
        OA_CHECK(listing.brains[4].manifest.id == "steady-commander");
        OA_CHECK(listing.brains[5].manifest.id == "wide-watch");
    }
    OA_CHECK(lib::find(listing, "calm-line") != nullptr);
    OA_CHECK(lib::find(listing, "lantern-scout") != nullptr);
    OA_CHECK(lib::find(listing, "other-id") != nullptr);
    OA_CHECK(lib::find(listing, "quiet-hold") != nullptr);
    OA_CHECK(lib::find(listing, "steady-commander") != nullptr);
    OA_CHECK(lib::find(listing, "wide-watch") != nullptr);
    OA_CHECK(lib::find(listing, "linked-away") == nullptr);
    OA_CHECK(lib::find(listing, "hidden-backup") == nullptr);
    OA_CHECK(lib::find(listing, "staged") == nullptr);
    OA_CHECK(lib::find(listing, "broken-one") == nullptr);
    OA_CHECK(lib::find(listing, "no-such") == nullptr);

    const lib::InstalledBrain* calm_found = lib::find(listing, "calm-line");
    OA_CHECK(calm_found != nullptr && calm_found->folder_name == "calm-line");
    OA_CHECK(calm_found != nullptr && calm_found->package_sha256 == "abc123");
    OA_CHECK(calm_found != nullptr && calm_found->registry == "ridge");
    OA_CHECK(calm_found != nullptr && calm_found->catalogue_id == "calm-line");
    OA_CHECK(calm_found != nullptr && calm_found->release == 7);
    OA_CHECK(calm_found != nullptr && calm_found->manifest.name == "Calm Line");
    OA_CHECK(calm_found != nullptr && calm_found->network_bytes == 350);

    const lib::InstalledBrain* lantern_found = lib::find(listing, "lantern-scout");
    OA_CHECK(lantern_found != nullptr && lantern_found->network_bytes == 12345);

    const lib::InstalledBrain* steady = lib::find(listing, "steady-commander");
    OA_CHECK(steady != nullptr && steady->folder_name == "steady-commander");
    OA_CHECK(steady != nullptr && steady->package_sha256 == "def456");
    OA_CHECK(steady != nullptr && steady->registry.empty());
    OA_CHECK(steady != nullptr && steady->catalogue_id.empty());
    OA_CHECK(steady != nullptr && steady->release == 0);
    OA_CHECK(steady != nullptr && steady->network_bytes == 0);

    const lib::InstalledBrain* wide_found = lib::find(listing, "wide-watch");
    OA_CHECK(wide_found != nullptr && wide_found->package_sha256.empty());
    OA_CHECK(wide_found != nullptr && wide_found->registry.empty());
    OA_CHECK(wide_found != nullptr && wide_found->release == 0);
    OA_CHECK(wide_found != nullptr && wide_found->network_bytes == 0);

    const lib::InstalledBrain* quiet = lib::find(listing, "quiet-hold");
    OA_CHECK(quiet != nullptr && quiet->package_sha256.empty());
    OA_CHECK(quiet != nullptr && quiet->registry.empty());

    const lib::InstalledBrain* other = lib::find(listing, "other-id");
    OA_CHECK(other != nullptr && other->folder_name == "alpha-copy");

    OA_CHECK(problem_has(listing, "broken-one: oabrain.yaml:"));
    OA_CHECK(problem_has(listing, "tags"));
    OA_CHECK(problem_has(listing, "empty-one: oabrain.yaml: it cannot be read"));
    OA_CHECK(problem_has(listing, "huge-yaml: oabrain.yaml: it is larger than 256 KiB"));
    OA_CHECK(
        problem_has(listing, "hands-copy: it has the same id steady-commander as steady-commander")
    );
    OA_CHECK(problem_has(listing, "beta-copy: it has the same id other-id as alpha-copy"));
    OA_CHECK(!problem_has(listing, "staged"));
    OA_CHECK(!problem_has(listing, "hidden-backup"));
    if (!link_error)
        OA_CHECK(!problem_has(listing, "linked-away") && !problem_has(listing, "linked-hands"));

    OA_CHECK(fs::file_size(notes) == note.size());
    {
        std::ifstream in(notes, std::ios::binary);
        const std::string kept{std::istreambuf_iterator<char>{in}, {}};
        OA_CHECK(kept == note);
    }

    const lib::Listing missing = lib::list_installed(scratch.path() / "no-brains");
    OA_CHECK(missing.brains.empty());
    OA_CHECK(missing.problems.empty());
}

} // namespace

int main() {
    const Scratch scratch;
    test_listing(scratch);
    return oa::test::check_exit_status();
}
