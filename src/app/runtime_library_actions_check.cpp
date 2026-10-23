// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-library-install: made-up mod packages posted to the installer as
// the download queue hands them over, each with a catalogue origin, while
// the Library shows over a feed of three made-up registries: check-builtin,
// built in, and check-added and check-added-2, added. No network is reached.
// Each run of the check is one turn. The first: an added registry's mod
// installs with no question and writes its origin record, and its next
// release replaces it keeping the first; the same key from the other added
// registry asks REPLACE MOD over the Library, CANCEL changing nothing and
// REPLACE replacing; the built-in registry's replaces it with no question
// and no prompt, and ROLL BACK's question over the Library brings the added
// registry's copy back; the built-in registry's replaces a file's other
// version with no question; a folder of the player's own is refused in the
// Library's queue, with MOD NOT INSTALLED once the Library is closed, and
// asks FOLDER IN USE from an added registry; a file still asks UPDATE MOD;
// a package that does not resolve is refused in the queue; then PLAY NOW.
// The second plays the mod: ROLL BACK's question says the game reloads, a
// built-in update to it asks nothing and waits, held, for the player to
// leave the mod, CANCEL on that line drops it and the update is taken again
// and held, and PLAY NOW switches to another mod; the third finds the
// update put in place as the mod was switched, with no notice, then asks for made-up updates:
// an added registry's whose rules change asks the update question first, NOT
// NOW leaving it and UPDATE queuing it, and one that keeps the rules is
// queued at once; the built-in registry's whose rules change, or cannot be
// known, are queued at once, their status lines and Rules facts saying so
// before, and again each in turn, the header's refresh icon offered once a registry can be
// asked; and UPDATE ALL queues the built-in one at once and asks about the
// added one only; then UNINSTALL: off for a map pack during a game, a notice
// for the language pack in use and for the mod being played, each keeping
// its folder, the question for a built-in registry's mod, CANCEL removing
// nothing and UNINSTALL its folder with the copy ROLL BACK keeps, its row
// staying not installed, and a mod of the player's own leaving the Library.
// The fourth plays No Mod. GET WITH on Gatehouse Keep asks, CANCEL queues
// nothing and GET BOTH queues Gatehouse then Gatehouse Keep; with both
// installed, uninstalling Gatehouse says Gatehouse Keep needs it. The
// folders are then removed. Fast Builders follows: GET queues it, the
// package installs under Mutators with its origin record. A .oamut handed
// over as the system opens one, a dropped file, asks INSTALL MUTATOR, lands
// under Mutators with a file origin, and the Library lists it; that folder
// is then removed so Fast Builders is the row again. CHECK
// WITH THIS MOD runs the fixture's fake checker and shows its verdict,
// OPEN FOLDER and HOMEPAGE reach the recorders, and UNINSTALL asks and
// removes the folder. The runtime's own verdict entries then dry-run made-up
// packages on the Library's worker against the installed game: one that
// changes every builder, one whose rule matches no unit, and one that
// declares only the maps layer. Three made-up brains follow: GET installs
// Steady Commander under Brains, USE BY DEFAULT writes the preference,
// CHECK WITH THIS MOD runs each brain's fits on the Library's worker, and
// UNINSTALL of the default clears the key. About forty more packages with
// origin records then take one listing on a rebuild. The fifth is
// a run nobody watches, on SDL's dummy
// video driver without the prompts the other runs show: a built-in registry's
// update waits in the inbox while the Library is closed, then installs with
// no prompt once the Library shows, the folder it drops deleted; an added
// registry's copy asks REPLACE MOD over the Library, which waits for its
// answer, and REPLACE installs it; and a file opened outside the game stays
// in the inbox.

#include "library_host.hpp"
#include "mod_install_state.hpp"
#include "oa_layer.hpp"
#include "oa_layer_check.hpp"
#include "user_folder_state.hpp"
#include "web_link_state.hpp"

#include "oa/app/content/downloads.hpp"
#include "oa/app/content/updates.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/app/mutators_memory.hpp"
#include "oa/app/package_install.hpp"
#include "oa/app/package_install/inbox.hpp"
#include "oa/app/package_install/oabrain.hpp"
#include "oa/app/package_install/oamod.hpp"
#include "oa/app/package_install/oamut.hpp"
#include "oa/app/package_install/origin.hpp"
#include "oa/app/runtime.hpp"
#include "oa/platform/preferences.hpp"
#include "oa/app/user_folder.hpp"
#include "oa/base/sha256.hpp"
#include "oa/data/mutators/verdicts.hpp"
#include "oa/formats/zip.hpp"
#include "oa/ui/display_layout.hpp"
#include "oa/ui/engine_settings/dialog.hpp"
#include "oa/ui/engine_settings/prompt.hpp"
#include "oa/ui/library/library.hpp"
#include "oa/ui/library/text.hpp"
#include "oa/ui/library/uninstall.hpp"
#include "oa/ui/library/update_question.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace oa::app {

namespace {

namespace settings = oa::ui::engine_settings;
namespace layout = oa::ui::display_layout;
namespace library = oa::ui::library;
namespace install = oa::app::package_install;

/// The made-up built-in registry.
constexpr std::string_view kBuiltIn = "check-builtin";
/// The first made-up registry the player added.
constexpr std::string_view kAdded = "check-added";
/// The second made-up registry the player added.
constexpr std::string_view kAddedOther = "check-added-2";
/// The made-up mod the check installs, and its folder: Ridge.
constexpr std::string_view kRidge = "ridge";
/// A made-up mod whose folder name a folder of the player's own holds.
constexpr std::string_view kCinder = "cinder";
/// A made-up mod the check switches to.
constexpr std::string_view kLattice = "lattice";
/// A made-up mod whose profile does not resolve.
constexpr std::string_view kDunes = "dunes";
/// A made-up mod of an added registry whose update changes the rules.
constexpr std::string_view kEmber = "ember";
/// A made-up mod of an added registry whose update keeps the rules.
constexpr std::string_view kFlint = "flint";
/// A made-up mod of the built-in registry the check uninstalls.
constexpr std::string_view kBasalt = "basalt";
/// A made-up mod of the player's own, no registry listing it, the check uninstalls.
constexpr std::string_view kScree = "scree";
/// A made-up map pack of the built-in registry, installed while a game is played.
constexpr std::string_view kArchipelago = "archipelago";
/// A made-up language pack of the built-in registry, the one in use.
constexpr std::string_view kPseudo = "en-XA";
/// A made-up mutator the fourth run gets, checks and uninstalls.
constexpr std::string_view kFast = "fast-builders";
/// A made-up mutator whose rule changes every builder, checked for real.
constexpr std::string_view kBuilders = "ridge-check-builders";
/// A made-up mutator whose rule matches nothing, checked for real.
constexpr std::string_view kQuiet = "ridge-check-quiet";
/// A made-up mutator Gatehouse Keep needs, downloaded with it.
constexpr std::string_view kGatehouse = "gatehouse";
/// A made-up mutator that needs Gatehouse.
constexpr std::string_view kGatehouseKeep = "gatehouse-keep";
/// A made-up brain with no fits. GET installs it, and it becomes the default.
constexpr std::string_view kSteady = "steady-commander";
/// A made-up brain whose fits returns a reason.
constexpr std::string_view kPicky = "picky-planner";
/// A made-up brain whose fits raises an error.
constexpr std::string_view kBroken = "broken-planner";
/// The most frames a step of the check runs before it gives up.
constexpr int kMostFrames = 400;
/// The frames a step runs to show that something waits: nothing comes of it.
constexpr int kWaitingFrames = 30;
/// The run of the check that nobody watches, the last: the fifth.
constexpr uint32_t kUnwatchedRun = 4;
/// What the download queue says of a package the installer refused, which
/// the installer's own reason replaces in the Library.
constexpr std::string_view kQueueRefused = "The package was refused";
/// Where the pointer rests: over no control of the main menu.
constexpr layout::Point kRestingPointer{8, 8};

/// Stops the check.
///
/// @param ok the step held
/// @param what what failed
void require(bool ok, std::string_view what) {
    if (!ok)
        throw std::runtime_error("library install check: " + std::string(what));
}

/// One package the fixture feed was asked to queue.
struct FeedQueued {
    std::string registry{};           ///< its registry
    std::string key{};                ///< its key
    content::DownloadReason reason{}; ///< why
};

/// The fixture feed the check gives the Library: its inputs, which the check
/// changes as the download queue and the player's folder would, and what it
/// was asked to queue, which it records and starts no download for.
struct InstallFeed {
    library::Inputs inputs{};         ///< what the Library is built from
    uint64_t generation{1};           ///< moves whenever the check changes inputs
    std::vector<FeedQueued> queued{}; ///< what it was asked to queue, in order
    /// Verdicts `keep_verdict` stored, keyed by the mutator's id.
    std::map<std::string, oa::data::mutators::VerdictRecord> kept{};
    int keep_count{0}; ///< how many times `keep_verdict` was called
    /// The verdict the running check returns once `release` is set.
    oa::data::mutators::VerdictRecord canned{};
    std::atomic<bool> release{false};      ///< the check may return `canned`
    std::atomic<bool> work_started{false}; ///< the worker has entered the check
    /// The runtime, for the real verdict entries swapped in after the fixture.
    Runtime* runtime{};
};

/// The fixture's inputs.
///
/// @param context the fixture
/// @return its inputs
library::Inputs feed_inputs(void* context) {
    return static_cast<InstallFeed*>(context)->inputs;
}

/// The fixture's generation.
///
/// @param context the fixture
/// @return the generation
uint64_t feed_generation(void* context) {
    return static_cast<InstallFeed*>(context)->generation;
}

/// Records a package the fixture was asked to queue.
///
/// @param context the fixture
/// @param registry the registry's id
/// @param key the package's key
/// @param reason why
/// @return a queue item's number: one more for each package recorded
std::optional<uint64_t> feed_enqueue(
    void* context,
    std::string_view registry,
    std::string_view key,
    content::DownloadReason reason,
    std::string* /*why*/
) {
    auto& fixture = *static_cast<InstallFeed*>(context);
    fixture.queued.push_back({std::string(registry), std::string(key), reason});
    return static_cast<uint64_t>(fixture.queued.size());
}

/// Tells whether a registry of the fixture's inputs is built in: the
/// Registry::built_in its inputs give.
///
/// @param context the fixture
/// @param registry the registry's id
/// @return true for check-builtin
bool feed_built_in(void* context, std::string_view registry) {
    for (const library::Registry& listed : static_cast<InstallFeed*>(context)->inputs.registries)
        if (listed.id == registry)
            return listed.built_in;
    return false;
}

/// The verdict `keep_verdict` stored for a mutator. Called on the main thread.
///
/// @param context the fixture
/// @param id the mutator's id
/// @return the record, or nothing when none was kept
std::optional<oa::data::mutators::VerdictRecord>
feed_remembered(void* context, std::string_view id) {
    const InstallFeed& fixture = *static_cast<const InstallFeed*>(context);
    const auto found = fixture.kept.find(std::string(id));
    if (found == fixture.kept.end())
        return std::nullopt;
    return found->second;
}

/// Stores a verdict the worker found. Called on the main thread.
///
/// @param context the fixture
/// @param id the mutator's id
/// @param record the verdict
void feed_keep(
    void* context, std::string_view id, const oa::data::mutators::VerdictRecord& record
) {
    InstallFeed& fixture = *static_cast<InstallFeed*>(context);
    fixture.kept.insert_or_assign(std::string(id), record);
    ++fixture.keep_count;
}

/// Starts a fake check. The work runs on the Library's worker: it waits until
/// the fixture releases it, and returns nothing when the check is cancelled.
///
/// @param context the fixture
/// @param id the mutator's id; the canned verdict does not depend on it
/// @return the work, which reads `canned` once it is released
std::function<std::optional<oa::data::mutators::VerdictRecord>(const std::atomic<bool>&)>
feed_start(void* context, std::string_view /*id*/) {
    auto* fixture = static_cast<InstallFeed*>(context);
    return [fixture](
               const std::atomic<bool>& cancel
           ) -> std::optional<oa::data::mutators::VerdictRecord> {
        fixture->work_started.store(true);
        while (!fixture->release.load() && !cancel.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (cancel.load())
            return std::nullopt;
        return fixture->canned;
    };
}

/// Returns one of the fixture's registries.
///
/// @param id its id
/// @param name its name
/// @param built_in it ships with the game
/// @return the registry
library::Registry feed_registry(std::string_view id, std::string_view name, bool built_in) {
    library::Registry registry;
    registry.id = std::string(id);
    registry.name = std::string(name);
    registry.built_in = built_in;
    return registry;
}

/// Returns a made-up profile.
///
/// @param id its id
/// @param name its name
/// @param version its version
/// @param revision its packaging revision
/// @param more lines after it
/// @return its text
std::string profile_text(
    std::string_view id,
    std::string_view name,
    std::string_view version,
    int revision,
    std::string_view more = {}
) {
    return "oamod: 1\nid: " + std::string(id) + "\nname: " + std::string(name) + "\nversion: \"" +
           std::string(version) +
           "\"\ndescription: A made-up mod the Library install check installs.\n"
           "requires: {base: ta-3.1c, catalogue: 1}\n"
           "author: {name: unknown}\n"
           "packaging: {revision: " +
           std::to_string(revision) + ", date: 2026-10-11, packager: Open Annihilation}\n" +
           std::string(more);
}

/// Writes a package of stored entries, its profile at its top.
///
/// @param file the package
/// @param profile its oamod.yaml
/// @param revision written into its one other file
void write_package(const fs::path& file, const std::string& profile, int revision) {
    const std::string readme = "revision " + std::to_string(revision);
    const std::vector<uint8_t> manifest(profile.begin(), profile.end());
    const std::vector<uint8_t> text(readme.begin(), readme.end());
    const std::vector<oa::formats::zip::NewEntry> listed{
        {"oamod.yaml", manifest},
        {"units/readme.txt", text},
    };
    std::vector<uint8_t> archive;
    oa::formats::zip::ZipError error{};
    require(oa::formats::zip::write_archive(listed, archive, error), "a package could not be made");
    std::error_code made;
    fs::create_directories(file.parent_path(), made);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(
        reinterpret_cast<const char*>(archive.data()), static_cast<std::streamsize>(archive.size())
    );
    require(static_cast<bool>(out), "a package could not be written");
}

/// Writes Fast Builders as a small .oamut: its manifest, its entry and a note.
///
/// @param file the package
void write_mutator(const fs::path& file) {
    const std::string manifest = "oamut: 1\n"
                                 "id: fast-builders\n"
                                 "name: \"Fast Builders\"\n"
                                 "version: \"1.0\"\n"
                                 "author:\n"
                                 "  name: \"Ada\"\n"
                                 "description: \"Builds faster.\"\n"
                                 "tags: [\"economy\"]\n"
                                 "works-with: [\"*\"]\n"
                                 "layers: [\"units\"]\n"
                                 "entry: \"mutator.lua\"\n";
    const std::string script = "return {}\n";
    const std::string note = "Fast Builders\n";
    const std::vector<uint8_t> manifest_bytes(manifest.begin(), manifest.end());
    const std::vector<uint8_t> script_bytes(script.begin(), script.end());
    const std::vector<uint8_t> note_bytes(note.begin(), note.end());
    const std::vector<oa::formats::zip::NewEntry> listed{
        {"oamut.yaml", manifest_bytes},
        {"mutator.lua", script_bytes},
        {"README.md", note_bytes},
    };
    std::vector<uint8_t> archive;
    oa::formats::zip::ZipError error{};
    require(oa::formats::zip::write_archive(listed, archive, error), "a mutator could not be made");
    std::error_code made;
    fs::create_directories(file.parent_path(), made);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(
        reinterpret_cast<const char*>(archive.data()), static_cast<std::streamsize>(archive.size())
    );
    require(static_cast<bool>(out), "a mutator could not be written");
}

/// Writes a made-up mutator package: its manifest, its entry and a note.
///
/// @param file the package
/// @param id its id
/// @param name its name
/// @param version its version
/// @param requires_yaml lines inserted after works-with, or none
void write_named_mutator(
    const fs::path& file,
    std::string_view id,
    std::string_view name,
    std::string_view version,
    std::string_view requires_yaml
) {
    const std::string manifest = "oamut: 1\n"
                                 "id: " +
                                 std::string(id) +
                                 "\n"
                                 "name: \"" +
                                 std::string(name) +
                                 "\"\n"
                                 "version: \"" +
                                 std::string(version) +
                                 "\"\n"
                                 "author:\n"
                                 "  name: \"Ada\"\n"
                                 "description: \"A made-up mutator.\"\n"
                                 "tags: [\"gameplay\"]\n"
                                 "works-with: [\"*\"]\n" +
                                 std::string(requires_yaml) +
                                 "layers: [\"files\"]\n"
                                 "entry: \"mutator.lua\"\n";
    const std::string script = "return {}\n";
    const std::string note = std::string(name) + "\n";
    const std::vector<uint8_t> manifest_bytes(manifest.begin(), manifest.end());
    const std::vector<uint8_t> script_bytes(script.begin(), script.end());
    const std::vector<uint8_t> note_bytes(note.begin(), note.end());
    const std::vector<oa::formats::zip::NewEntry> listed{
        {"oamut.yaml", manifest_bytes},
        {"mutator.lua", script_bytes},
        {"README.md", note_bytes},
    };
    std::vector<uint8_t> archive;
    oa::formats::zip::ZipError error{};
    require(oa::formats::zip::write_archive(listed, archive, error), "a mutator could not be made");
    std::error_code made;
    fs::create_directories(file.parent_path(), made);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(
        reinterpret_cast<const char*>(archive.data()), static_cast<std::streamsize>(archive.size())
    );
    require(static_cast<bool>(out), "a mutator could not be written");
}

/// Writes a made-up mutator whose script is a real rule, for a dry run.
///
/// @param file the package
/// @param id its id
/// @param name its name
/// @param lua its entry
/// @param layers the one layer it declares
void write_checking_mutator(
    const fs::path& file,
    std::string_view id,
    std::string_view name,
    std::string_view lua,
    std::string_view layers = "units"
) {
    const std::string manifest = "oamut: 1\n"
                                 "id: " +
                                 std::string(id) +
                                 "\n"
                                 "name: \"" +
                                 std::string(name) +
                                 "\"\n"
                                 "version: \"1.0\"\n"
                                 "author:\n"
                                 "  name: \"Ridge Maker\"\n"
                                 "description: \"A made-up mutator.\"\n"
                                 "tags: [\"gameplay\"]\n"
                                 "works-with: [\"*\"]\n"
                                 "layers: [\"" +
                                 std::string(layers) +
                                 "\"]\n"
                                 "entry: \"mutator.lua\"\n";
    const std::vector<uint8_t> manifest_bytes(manifest.begin(), manifest.end());
    const std::vector<uint8_t> script_bytes(lua.begin(), lua.end());
    const std::string note = std::string(name) + "\n";
    const std::vector<uint8_t> note_bytes(note.begin(), note.end());
    const std::vector<oa::formats::zip::NewEntry> listed{
        {"oamut.yaml", manifest_bytes},
        {"mutator.lua", script_bytes},
        {"README.md", note_bytes},
    };
    std::vector<uint8_t> archive;
    oa::formats::zip::ZipError error{};
    require(oa::formats::zip::write_archive(listed, archive, error), "a mutator could not be made");
    std::error_code made;
    fs::create_directories(file.parent_path(), made);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(
        reinterpret_cast<const char*>(archive.data()), static_cast<std::streamsize>(archive.size())
    );
    require(static_cast<bool>(out), "a mutator could not be written");
}

/// Writes a made-up brain package: its manifest and its entry.
///
/// @param file the package
/// @param id its id
/// @param name its name
/// @param lua its entry
void write_brain(
    const fs::path& file, std::string_view id, std::string_view name, std::string_view lua
) {
    const std::string manifest = "oabrain: 1\n"
                                 "id: " +
                                 std::string(id) +
                                 "\n"
                                 "name: \"" +
                                 std::string(name) +
                                 "\"\n"
                                 "version: \"1.0\"\n"
                                 "author:\n"
                                 "  name: \"Ridge Maker\"\n"
                                 "description: \"A made-up brain.\"\n"
                                 "tags: [\"classic\"]\n"
                                 "works-with: [\"*\"]\n"
                                 "levels:\n"
                                 "  - id: steady\n"
                                 "    name: \"Steady\"\n"
                                 "    difficulty: easy\n"
                                 "entry: \"brain.lua\"\n";
    const std::vector<uint8_t> manifest_bytes(manifest.begin(), manifest.end());
    const std::vector<uint8_t> script_bytes(lua.begin(), lua.end());
    const std::string note = std::string(name) + "\n";
    const std::vector<uint8_t> note_bytes(note.begin(), note.end());
    const std::vector<oa::formats::zip::NewEntry> listed{
        {"oabrain.yaml", manifest_bytes},
        {"brain.lua", script_bytes},
        {"README.md", note_bytes},
    };
    std::vector<uint8_t> archive;
    oa::formats::zip::ZipError error{};
    require(oa::formats::zip::write_archive(listed, archive, error), "a brain could not be made");
    std::error_code made;
    fs::create_directories(file.parent_path(), made);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(
        reinterpret_cast<const char*>(archive.data()), static_cast<std::streamsize>(archive.size())
    );
    require(static_cast<bool>(out), "a brain could not be written");
}

/// Copies requires.mutators from an installed mutator's manifest into its facts.
///
/// A line that is not a flow list is left unread. Fast Builders' manifest
/// names none, so its facts stay empty of needs.
///
/// @param folder the installed folder
/// @param[in,out] facts the facts; needs are appended
void read_mutator_needs(const fs::path& folder, library::MutatorFacts& facts) {
    std::ifstream manifest(folder / "oamut.yaml");
    if (!manifest)
        return;
    std::string line;
    while (std::getline(manifest, line)) {
        const std::string::size_type marker = line.find("mutators:");
        if (marker == std::string::npos)
            continue;
        std::string::size_type cursor = marker;
        for (;;) {
            const std::string::size_type open = line.find('"', cursor);
            if (open == std::string::npos)
                break;
            const std::string::size_type close = line.find('"', open + 1);
            if (close == std::string::npos)
                break;
            const std::string text = line.substr(open + 1, close - open - 1);
            library::Prerequisite need;
            const std::string::size_type space = text.find(' ');
            if (space == std::string::npos) {
                need.id = text;
            } else {
                need.id = text.substr(0, space);
                need.range = text.substr(space + 1);
            }
            if (!need.id.empty())
                facts.needs.push_back(std::move(need));
            cursor = close + 1;
        }
    }
}

/// Returns the SHA-256 of a file.
///
/// @param file the file
/// @return its digest
oa::base::sha256::Digest file_hash(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>{in}, {}};
    return oa::base::sha256::digest_of(bytes);
}

/// Returns the revision a folder's profile gives; -1 when it holds no mod.
///
/// @param folder the folder
/// @return the revision
int64_t revision_in(const fs::path& folder) {
    const auto held = install::oamod::read_installed_mod(folder);
    return held.kind == install::FolderKind::package ? held.revision : -1;
}

/// Tells whether a folder's origin record names a catalogue's release.
///
/// @param folder the folder
/// @param registry the registry
/// @param release the release
/// @return true when it does
bool recorded_from(const fs::path& folder, std::string_view registry, int64_t release) {
    const std::optional<install::Origin> record = install::read_origin(folder);
    return record && record->kind == install::OriginKind::catalogue &&
           record->registry == registry && record->catalogue_id == kRidge &&
           record->release == release;
}

/// Tells whether the Mods folder holds a folder an install keeps while it works.
///
/// @param mods the Mods folder
/// @return true when one is there
bool reserved_left(const fs::path& mods) {
    const install::FolderNames names = install::folder_names(install::mod_kind());
    std::error_code error;
    for (fs::directory_iterator entry{mods, error}, end; !error && entry != end;
         entry.increment(error)) {
        const std::string name = path_to_utf8(entry->path().filename());
        if (name.starts_with(names.reserved) && name != names.lock)
            return true;
    }
    return false;
}

/// Returns a folder's path as the settings keep it: absolute, normal, in UTF-8.
///
/// @param folder the folder
/// @return its path
std::string kept_path(const fs::path& folder) {
    std::error_code error;
    const fs::path absolute = fs::absolute(folder, error);
    return path_to_utf8((error ? folder : absolute).lexically_normal());
}

} // namespace

void Runtime::check_library_install() {
    require(sdl_.window != nullptr, "the check needs the SDL presenter");
    require(!user_folder_.empty(), "the run has no folder of the player's own");
    const fs::path mods = user_folder_ / std::string(user_mods_folder_name);
    const fs::path packages = user_folder_.parent_path() / "files";
    const fs::path ridge = mods / std::string(kRidge);
    const fs::path kept = ridge / std::string(install::backup_folder_name);
    auto& state = mod_install_state();
    // Every run but the last shows the prompts although nobody watches it;
    // the last shows only what a run nobody watches shows.
    state.check_shows_prompts = options_.restarts < kUnwatchedRun;

    // The Library takes the fixture's feed from the first frame on, and the
    // runtime's own back as the check ends.
    InstallFeed fixture;
    fixture.runtime = this;
    fixture.inputs.registries.push_back(feed_registry(kBuiltIn, "Check Built-in", true));
    fixture.inputs.registries.push_back(feed_registry(kAdded, "Check Added", false));
    fixture.inputs.registries.push_back(feed_registry(kAddedOther, "Check Added Two", false));
    fixture.inputs.engine = content::this_engine_rules().engine;
    fixture.inputs.version_text = "v0.8.0";
    LibraryHost& host = library_host();
    const LibraryFeed own = host.feed;

    struct OwnFeedBack {
        LibraryHost& host;
        LibraryFeed feed;

        ~OwnFeedBack() {
            host.feed = feed;
            host.own_feed = true;
            host.refreshed = false;
        }
    } own_back{host, own};

    host.feed = LibraryFeed{
        &fixture, feed_inputs, feed_generation, feed_enqueue, nullptr, nullptr, feed_built_in
    };
    host.own_feed = false;
    host.refreshed = false;

    bool running = true;
    // Installed packages of the other kinds the fixture lists after the mods.
    std::vector<library::Installed> beside_mods;
    // The installed mods as the Library's worker would list them: each
    // folder of Mods that holds a mod, its registry and key from its origin
    // record, the version its .backup keeps and whether the game plays it;
    // then the packages beside them.
    const auto list_installed = [&] {
        fixture.inputs.installed.clear();
        std::error_code error;
        for (fs::directory_iterator entry{mods, error}, end; !error && entry != end;
             entry.increment(error)) {
            const fs::path folder = entry->path();
            // The folders an install keeps while it works are not mods.
            if (path_to_utf8(folder.filename()).starts_with('.'))
                continue;
            const install::InstalledPackage held = install::oamod::read_installed_mod(folder);
            if (held.kind != install::FolderKind::package)
                continue;
            const std::optional<install::Origin> record = install::read_origin(folder);
            library::Installed shown;
            shown.kind = library::Kind::mod;
            shown.key = held.id;
            shown.folder = folder;
            shown.name = held.name;
            shown.version = held.version;
            shown.revision = held.revision;
            if (record && record->kind == install::OriginKind::catalogue) {
                shown.key = record->catalogue_id;
                shown.registry = record->registry;
                shown.release = record->release;
            }
            shown.playing = package_target_in_use(install::mod_kind(), folder);
            if (const auto backup = install::oamod::read_backup(folder);
                backup && backup->kind == install::FolderKind::package)
                shown.kept = library::Kept{backup->version, backup->revision};
            fixture.inputs.installed.push_back(std::move(shown));
        }
        error.clear();
        const fs::path mutators_root =
            user_folder_ / std::string(install::oamut::mutators_folder_name);
        if (fs::is_directory(mutators_root, error)) {
            error.clear();
            for (fs::directory_iterator entry{mutators_root, error}, end; !error && entry != end;
                 entry.increment(error)) {
                const fs::path folder = entry->path();
                if (path_to_utf8(folder.filename()).starts_with('.'))
                    continue;
                const install::InstalledPackage held = install::oamut::read_installed(folder);
                if (held.kind != install::FolderKind::package)
                    continue;
                const std::optional<install::Origin> record = install::read_origin(folder);
                library::Installed shown;
                shown.kind = library::Kind::mutator;
                shown.key = held.id;
                shown.folder = folder;
                shown.name = held.name;
                shown.version = held.version;
                shown.revision = held.revision;
                shown.author = held.author;
                shown.description = held.description;
                shown.mutator = library::MutatorFacts{};
                read_mutator_needs(folder, *shown.mutator);
                if (record && record->kind == install::OriginKind::catalogue) {
                    shown.key = record->catalogue_id;
                    shown.registry = record->registry;
                    shown.release = record->release;
                }
                fixture.inputs.installed.push_back(std::move(shown));
            }
        }
        error.clear();
        const fs::path brains_root =
            user_folder_ / std::string(install::oabrain::brains_folder_name);
        if (fs::is_directory(brains_root, error)) {
            error.clear();
            for (fs::directory_iterator entry{brains_root, error}, end; !error && entry != end;
                 entry.increment(error)) {
                const fs::path folder = entry->path();
                if (path_to_utf8(folder.filename()).starts_with('.'))
                    continue;
                const install::InstalledPackage held = install::oabrain::read_installed(folder);
                if (held.kind != install::FolderKind::package)
                    continue;
                const std::optional<install::Origin> record = install::read_origin(folder);
                library::Installed shown;
                shown.kind = library::Kind::brain;
                shown.key = held.id;
                shown.folder = folder;
                shown.name = held.name;
                shown.version = held.version;
                shown.revision = held.revision;
                shown.author = held.author;
                shown.description = held.description;
                // Empty works-with is any mod. The check attaches the verdict.
                shown.brain = library::BrainFacts{};
                if (record && record->kind == install::OriginKind::catalogue) {
                    shown.key = record->catalogue_id;
                    shown.registry = record->registry;
                    shown.release = record->release;
                }
                fixture.inputs.installed.push_back(std::move(shown));
            }
        }
        fixture.inputs.installed.insert(
            fixture.inputs.installed.end(), beside_mods.begin(), beside_mods.end()
        );
        ++fixture.generation;
    };
    const auto frame = [&] {
        run_frame(running);
        require(running, "a frame ended the run");
    };
    // Two frames: the Library's model follows its feed in the first, after
    // the OA layer's screens have ticked, and the Library lays itself out
    // again in the second.
    const auto frames = [&] {
        frame();
        frame();
    };
    const auto tap = [this](SDL_Keycode code) {
        bool going = true;
        for (const bool down : {true, false}) {
            SDL_Event event{};
            event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
            event.key.windowID = SDL_GetWindowID(sdl_.window);
            event.key.key = code;
            event.key.scancode = SDL_GetScancodeFromKey(code, nullptr);
            event.key.down = down;
            dispatch_event(event, going);
        }
        require(going, "a key ended the run");
    };
    const auto named = [this](std::string_view name) -> std::optional<AutomationControl> {
        std::vector<AutomationControl> controls;
        oa_layer().automation_controls(controls);
        for (const AutomationControl& control : controls)
            if (control.name == name && control.visible && control.window_width > 0 &&
                control.window_height > 0)
                return control;
        return std::nullopt;
    };
    const auto click_named = [&](std::string_view name) {
        const std::optional<AutomationControl> control = named(name);
        require(control.has_value(), std::string(name) + " is not shown");
        const layout::Point pixel{
            control->window_x + control->window_width / 2,
            control->window_y + control->window_height / 2
        };
        for (const SDL_EventType type :
             {SDL_EVENT_MOUSE_MOTION, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP}) {
            SDL_Event event = window_pointer_event(
                sdl_.window,
                sdl_.renderer,
                type,
                pixel,
                type == SDL_EVENT_MOUSE_MOTION ? 0 : SDL_BUTTON_LEFT
            );
            bool going = true;
            dispatch_event(event, going);
            require(going, "a click ended the run");
        }
        send_check_pointer(SDL_EVENT_MOUSE_MOTION, kRestingPointer, 0);
    };
    const auto top_is = [this](std::string_view name) {
        const LayerScreen* top = oa_layer().top();
        return top != nullptr && top->name() == name;
    };
    // A few frames on the main menu; a notice nobody asked for is closed.
    const auto settle = [&] {
        for (int count = 0; count < 12; ++count) {
            frame();
            const LayerScreen* top = oa_layer().top();
            if (top != nullptr && dynamic_cast<const NoticeScreen*>(top) != nullptr)
                tap(SDLK_RETURN);
            else if (top == nullptr && count >= 2)
                return;
        }
        require(oa_layer().top() == nullptr, "a screen stayed over the main menu");
    };
    const auto open_the_library = [&] {
        list_installed();
        open_library(library::Tab::mods);
        frames();
        require(top_is(library_screen_name), "the Library did not open over the main menu");
    };
    const auto close_the_library = [&] {
        close_library();
        frame();
        require(oa_layer().top() == nullptr, "the Library did not close");
    };
    // Shows an entry's details: its row selected, and at Compact its page.
    const auto show_entry = [&](std::string_view row) {
        if (named("oa.library.back"))
            click_named("oa.library.back");
        click_named(row);
    };
    // Presses one of the shown entry's actions: a button, or an item of
    // MORE ▾ at Compact.
    const auto press_action = [&](std::string_view word) {
        const std::string action = "oa.library.action." + std::string(word);
        if (!named(action))
            click_named("oa.library.action");
        click_named(action);
    };
    // The queue item the Library shows for a package of a registry.
    const auto queued = [&](std::string_view registry,
                            std::string_view key) -> std::optional<library::QueueItem> {
        for (const library::QueueItem& item : host.model.inputs.queue)
            if (item.registry == registry && item.key == key)
                return item;
        return std::nullopt;
    };
    // Posts a package as the download queue's hand-off does, with its
    // catalogue origin, and lists it in the fixture's queue as installing.
    const auto post_catalogue = [&](const fs::path& file,
                                    std::string_view registry,
                                    std::string_view key,
                                    std::string_view name,
                                    int64_t release,
                                    library::Kind kind = library::Kind::mod) {
        install::Origin origin{};
        origin.kind = install::OriginKind::catalogue;
        origin.registry = std::string(registry);
        origin.catalogue_id = std::string(key);
        origin.release = release;
        origin.sha256 = file_hash(file);
        install::post_package_file(file, origin);
        auto& queue = fixture.inputs.queue;
        queue.erase(
            std::remove_if(
                queue.begin(),
                queue.end(),
                [&](const library::QueueItem& item) {
                    return item.registry == registry && item.key == key;
                }
            ),
            queue.end()
        );
        library::QueueItem item;
        item.registry = std::string(registry);
        item.kind = kind;
        item.key = std::string(key);
        item.name = std::string(name);
        item.phase = library::QueueItem::Phase::installing;
        queue.push_back(std::move(item));
        ++fixture.generation;
    };
    // Writes Ridge at a version and revision and posts it from a registry.
    const auto post_ridge =
        [&](std::string_view registry, std::string_view version, int revision, int64_t release) {
            const fs::path file = packages / ("ridge-" + std::string(registry) + "-" +
                                              std::to_string(release) + ".oamod");
            write_package(file, profile_text(kRidge, "Ridge", version, revision), revision);
            post_catalogue(file, registry, kRidge, "Ridge", release);
        };
    // The installer's frames, as the main loop runs them, until the package
    // posted has an outcome, which is taken as the download queue takes it,
    // or a question shows.
    const auto until_outcome =
        [&](std::string_view what) -> std::optional<install::PackageOutcome> {
        for (int count = 0; count < kMostFrames; ++count) {
            tell_mod_installs();
            std::vector<install::PackageOutcome> outcomes = install::take_package_outcomes();
            if (!outcomes.empty())
                return outcomes.front();
            if (state.prompt != nullptr && state.stage == ModInstallState::Stage::asking)
                return std::nullopt;
        }
        require(false, std::string("nothing came of ") + std::string(what));
        return std::nullopt;
    };
    // What the download queue makes of an outcome: an installed item, a
    // failed one with its own words, or a cancelled one, which the Library
    // leaves out; the player's folder listed again and a frame drawn.
    const auto take_outcome = [&](const install::PackageOutcome& outcome,
                                  std::string_view registry,
                                  std::string_view key) {
        auto& queue = fixture.inputs.queue;
        for (auto item = queue.begin(); item != queue.end(); ++item) {
            if (item->registry != registry || item->key != key)
                continue;
            if (outcome.result == install::OutcomeResult::installed) {
                item->phase = library::QueueItem::Phase::installed;
            } else if (outcome.result == install::OutcomeResult::set_aside) {
                queue.erase(item);
            } else {
                item->phase = library::QueueItem::Phase::failed;
                item->problem = std::string(kQueueRefused);
                item->retryable = true;
            }
            break;
        }
        list_installed();
        frames();
    };
    // A catalogue package installed with no question and no prompt.
    const auto installs_quietly =
        [&](std::string_view what, std::string_view registry, std::string_view key) {
            const uint32_t shown = state.prompts_shown;
            const std::optional<install::PackageOutcome> outcome = until_outcome(what);
            require(outcome.has_value(), std::string(what) + " asked a question");
            require(
                outcome->result == install::OutcomeResult::installed,
                std::string(what) + " was not installed: " + outcome->reason
            );
            require(
                state.prompts_shown == shown && state.prompt == nullptr,
                std::string(what) + " showed a prompt"
            );
            take_outcome(*outcome, registry, key);
        };
    // A catalogue package's question, pushed over the Library at once.
    const auto asks_over_library = [&](std::string_view what, std::string_view title) {
        const std::optional<install::PackageOutcome> outcome = until_outcome(what);
        require(!outcome.has_value(), std::string(what) + " asked no question");
        require(
            state.prompt != nullptr && oa_layer().top() == state.prompt &&
                oa_layer().find(library_screen_name) != nullptr &&
                state.prompt->question().title == title,
            std::string(what) + " did not ask " + std::string(title) + " over the Library"
        );
    };
    // Frames over the main menu until a prompt that asks or tells shows.
    const auto until_prompt = [&](std::string_view what) {
        for (int count = 0; count < kMostFrames; ++count) {
            tell_mod_installs();
            if (state.prompt != nullptr && (state.stage == ModInstallState::Stage::asking ||
                                            state.stage == ModInstallState::Stage::telling))
                return;
        }
        require(false, std::string("no prompt showed: ") + std::string(what));
    };
    const auto says = [&](std::string_view part) {
        if (state.prompt == nullptr)
            return false;
        for (const auto& paragraph : state.prompt->question().paragraphs)
            if (paragraph.text.find(part) != std::string::npos)
                return true;
        return false;
    };
    const auto titled = [&](std::string_view title) {
        return state.prompt != nullptr && state.prompt->question().title == title;
    };
    const auto playing = [&](const fs::path& folder) {
        return !options_.layered_mod_folder.empty() &&
               kept_path(options_.layered_mod_folder) == kept_path(folder);
    };

    load(Screen::main_menu);
    require(screen_ == Screen::main_menu, "the main menu did not open");
    require(engine_settings_fonts() != nullptr, "the dialog's fonts did not load");
    send_check_pointer(SDL_EVENT_MOUSE_MOTION, kRestingPointer, 0);

    switch (options_.restarts) {
    case 0: {
        // The check starts afresh: an earlier run's packages and mods go.
        settle();
        require(
            options_.layered_mod_folder.empty(),
            "an earlier run of the check that stopped left its mod chosen; start it again"
        );
        std::error_code error;
        fs::remove_all(mods, error);
        fs::remove_all(packages, error);
        require(
            library_registry_built_in(kBuiltIn) && !library_registry_built_in(kAdded) &&
                !library_registry_built_in(kAddedOther),
            "library_registry_built_in does not answer as the feed's registries say"
        );
        open_the_library();

        // 1. An added registry's mod installs with no question, and its
        // folder records where it came from.
        post_ridge(kAdded, "1.0", 1, 1);
        installs_quietly("check-added's first release", kAdded, kRidge);
        require(
            revision_in(ridge) == 1 && recorded_from(ridge, kAdded, 1),
            "check-added's first release is not in Mods with its origin record"
        );
        require(top_is(library_screen_name), "the Library did not stay open over the install");

        // 2. Its next release replaces it with no question, keeping the first.
        post_ridge(kAdded, "1.0", 2, 2);
        installs_quietly("check-added's second release", kAdded, kRidge);
        require(
            revision_in(ridge) == 2 && revision_in(kept) == 1 && recorded_from(kept, kAdded, 1),
            "check-added's second release did not keep the first as the .backup"
        );
        require(
            queued(kAdded, kRidge) &&
                queued(kAdded, kRidge)->phase == library::QueueItem::Phase::installed,
            "the Library's queue does not say check-added's release is installed"
        );

        // 3. The same key from the other added registry asks REPLACE MOD
        // over the Library: CANCEL changes nothing, REPLACE replaces.
        post_ridge(kAddedOther, "1.0", 3, 1);
        asks_over_library("check-added-2's release", "REPLACE MOD");
        {
            const settings::Prompt& question = state.prompt->question();
            require(
                question.buttons.size() == 2 && question.buttons[0].id == "cancel" &&
                    question.buttons[1].id == "replace" && question.marked == 0 &&
                    says(
                        "Ridge from Check Added is installed. Replace it with Ridge from Check "
                        "Added Two? The one from Check Added is kept, and ROLL BACK brings it back."
                    ),
                "REPLACE MOD does not name both registries with CANCEL marked and REPLACE"
            );
            frame();
            require(
                named("oa.prompt.replace").has_value() && named("oa.prompt.cancel").has_value() &&
                    named("oa.prompt.body").has_value(),
                "automation does not list REPLACE MOD's oa.prompt.replace, cancel and body"
            );
        }
        tap(SDLK_ESCAPE);
        {
            const std::optional<install::PackageOutcome> outcome = until_outcome("CANCEL");
            require(
                outcome && outcome->result == install::OutcomeResult::set_aside &&
                    state.prompt == nullptr,
                "CANCEL on REPLACE MOD did not set the package aside"
            );
            take_outcome(*outcome, kAddedOther, kRidge);
        }
        require(
            revision_in(ridge) == 2 && recorded_from(ridge, kAdded, 2) && revision_in(kept) == 1,
            "CANCEL on REPLACE MOD changed the folder"
        );
        post_ridge(kAddedOther, "1.0", 3, 1);
        asks_over_library("check-added-2's release again", "REPLACE MOD");
        tap(SDLK_Y);
        {
            const std::optional<install::PackageOutcome> outcome = until_outcome("REPLACE");
            require(
                outcome && outcome->result == install::OutcomeResult::installed &&
                    state.prompt == nullptr,
                "REPLACE on REPLACE MOD did not install check-added-2's release"
            );
            take_outcome(*outcome, kAddedOther, kRidge);
        }
        require(
            revision_in(ridge) == 3 && recorded_from(ridge, kAddedOther, 1) &&
                recorded_from(kept, kAdded, 2),
            "REPLACE did not keep check-added's copy as the .backup"
        );

        // 4. The built-in registry's replaces it with no question and no
        // prompt, keeping the added registry's copy with its record, which
        // ROLL BACK's question over the Library brings back.
        post_ridge(kBuiltIn, "1.0", 4, 1);
        installs_quietly("check-builtin's release", kBuiltIn, kRidge);
        require(
            revision_in(ridge) == 4 && recorded_from(ridge, kBuiltIn, 1) &&
                revision_in(kept) == 3 && recorded_from(kept, kAddedOther, 1),
            "check-builtin's release did not keep check-added-2's copy with its record"
        );
        show_entry("oa.library.row.check-builtin.ridge");
        press_action("roll-back");
        {
            const auto* asked = dynamic_cast<const QuestionScreen*>(oa_layer().top());
            require(
                asked != nullptr && oa_layer().find(library_screen_name) != nullptr &&
                    engine_settings_dialog() == nullptr,
                "ROLL BACK did not ask its question over the Library"
            );
            const settings::Prompt& question = asked->question();
            require(
                question.title == "ROLL BACK MOD" && question.buttons.size() == 2 &&
                    question.buttons[0].id == "cancel" && question.buttons[1].id == "roll-back" &&
                    question.marked == 0 && question.paragraphs.size() == 1 &&
                    question.paragraphs[0].text ==
                        "Roll back Ridge to 1.0 revision 3? 1.0 revision 4 is kept in its place, "
                        "and ROLL BACK again brings it back.",
                "ROLL BACK's question is not the Roll Back Mod question for Ridge"
            );
        }
        tap(SDLK_ESCAPE);
        require(
            top_is(library_screen_name) && revision_in(ridge) == 4,
            "CANCEL on ROLL BACK's question changed the folder"
        );
        press_action("roll-back");
        tap(SDLK_Y);
        require(
            top_is(library_screen_name) && revision_in(ridge) == 3 &&
                recorded_from(ridge, kAddedOther, 1) && revision_in(kept) == 4 &&
                recorded_from(kept, kBuiltIn, 1) && !soft_restart_requested(),
            "ROLL BACK did not bring check-added-2's copy back with its record"
        );
        list_installed();
        frames();
        std::cout << "library install check: built-in package installed with no question\n";

        // 5. The built-in registry's replaces another version a file
        // installed, ANOTHER VERSION for a file, with no question.
        close_the_library();
        fs::remove_all(ridge, error);
        const fs::path other_version = packages / "ridge-2.0.oamod";
        write_package(other_version, profile_text(kRidge, "Ridge", "2.0", 1), 1);
        install::post_package_file(other_version);
        until_prompt("the file of another version");
        require(titled("MOD INSTALLED"), "the file of another version was not installed");
        tap(SDLK_RETURN);
        require(state.prompt == nullptr, "Enter did not close the file's notice");
        {
            const std::optional<install::Origin> record = install::read_origin(ridge);
            require(
                record && record->kind == install::OriginKind::file &&
                    install::oamod::read_installed_mod(ridge).version == "2.0",
                "the file of another version is not in Mods with a file origin"
            );
        }
        open_the_library();
        post_ridge(kBuiltIn, "1.0", 4, 1);
        installs_quietly("check-builtin's release over a file's other version", kBuiltIn, kRidge);
        require(
            revision_in(ridge) == 4 && recorded_from(ridge, kBuiltIn, 1) &&
                install::oamod::read_installed_mod(kept).version == "2.0",
            "check-builtin's release did not keep the file's other version as the .backup"
        );

        // 6. A folder of the player's own where a built-in registry's mod
        // goes is refused in the Library's queue with no question and no
        // prompt, and left as it is; an added registry's asks FOLDER IN USE;
        // with the Library closed, the refusal is told.
        const fs::path players_folder = mods / std::string(kCinder);
        fs::create_directories(players_folder, error);
        {
            std::ofstream(players_folder / "notes.txt") << "not a mod";
        }
        const fs::path cinder = packages / "cinder.oamod";
        write_package(cinder, profile_text(kCinder, "Cinder", "1.0", 1), 1);
        const std::string own_reason =
            "Mods/cinder is a folder of your own, not Cinder. Move or rename it, then RETRY.";
        {
            const uint32_t shown = state.prompts_shown;
            post_catalogue(cinder, kBuiltIn, kCinder, "Cinder", 1);
            const std::optional<install::PackageOutcome> outcome =
                until_outcome("check-builtin's Cinder");
            require(
                outcome && outcome->result == install::OutcomeResult::refused &&
                    state.prompts_shown == shown && state.prompt == nullptr,
                "check-builtin's Cinder over a folder of the player's own asked or showed a prompt"
            );
            take_outcome(*outcome, kBuiltIn, kCinder);
        }
        {
            const std::optional<library::QueueItem> item = queued(kBuiltIn, kCinder);
            require(
                item && item->phase == library::QueueItem::Phase::failed &&
                    item->problem == own_reason && item->retryable,
                "the Library's queue does not say the folder is the player's own"
            );
        }
        const auto own_folder_unchanged = [&] {
            return fs::exists(players_folder / "notes.txt") &&
                   !fs::exists(players_folder / "oamod.yaml") && !fs::exists(mods / "cinder-1.0");
        };
        require(own_folder_unchanged(), "the folder of the player's own was changed");
        post_catalogue(cinder, kAdded, kCinder, "Cinder", 1);
        asks_over_library("check-added's Cinder", "FOLDER IN USE");
        tap(SDLK_ESCAPE);
        {
            const std::optional<install::PackageOutcome> outcome = until_outcome("CANCEL");
            require(
                outcome && outcome->result == install::OutcomeResult::set_aside,
                "CANCEL on FOLDER IN USE did not set the package aside"
            );
            take_outcome(*outcome, kAdded, kCinder);
        }
        require(own_folder_unchanged(), "CANCEL on FOLDER IN USE changed the folder");
        close_the_library();
        post_catalogue(cinder, kBuiltIn, kCinder, "Cinder", 1);
        until_prompt("check-builtin's Cinder with the Library closed");
        require(
            titled("MOD NOT INSTALLED") && says(own_reason),
            "with the Library closed the folder's refusal was not told in MOD NOT INSTALLED"
        );
        tap(SDLK_RETURN);
        require(state.prompt == nullptr, "Enter did not close MOD NOT INSTALLED");
        std::ignore = install::take_package_outcomes();
        require(own_folder_unchanged(), "a refusal with the Library closed changed the folder");

        // 7. A file the player opens still asks UPDATE MOD.
        const fs::path file_update = packages / "ridge-1.0-r5.oamod";
        write_package(file_update, profile_text(kRidge, "Ridge", "1.0", 5), 5);
        install::post_package_file(file_update);
        until_prompt("a file of the next revision");
        require(titled("UPDATE MOD"), "a file of the next revision did not ask UPDATE MOD");
        tap(SDLK_ESCAPE);
        require(
            state.prompt == nullptr && revision_in(ridge) == 4,
            "CANCEL on UPDATE MOD changed the folder"
        );

        // 8. With the Library open, a refused package shows no prompt and
        // the Library's queue says why.
        open_the_library();
        const fs::path dunes = packages / "dunes.oamod";
        write_package(
            dunes,
            profile_text(kDunes, "Dunes", "0.9", 1, "hacks:\n  example.no-such-hack: true\n"),
            1
        );
        {
            const uint32_t shown = state.prompts_shown;
            post_catalogue(dunes, kAdded, kDunes, "Dunes", 1);
            const std::optional<install::PackageOutcome> outcome =
                until_outcome("check-added's Dunes");
            require(
                outcome && outcome->result == install::OutcomeResult::refused &&
                    state.prompts_shown == shown && state.prompt == nullptr,
                "a refused package showed a prompt over the Library"
            );
            take_outcome(*outcome, kAdded, kDunes);
        }
        {
            const std::optional<library::QueueItem> item = queued(kAdded, kDunes);
            require(
                item && item->phase == library::QueueItem::Phase::failed &&
                    item->problem.starts_with("Its oamod.yaml has errors: ") &&
                    item->problem.find("no-such-hack") != std::string::npos,
                "the Library's queue does not say why Dunes was refused"
            );
        }
        require(!fs::exists(mods / std::string(kDunes)), "a refused package was installed");

        // 11. PLAY NOW plays the mod: the run ends for it.
        show_entry("oa.library.row.check-builtin.ridge");
        press_action("play-now");
        require(soft_restart_requested(), "PLAY NOW did not end the run");
        const auto stored = preference_values_.find(std::string(settings::key::mod_directory));
        require(
            stored != preference_values_.end() && stored->second == kept_path(ridge),
            "PLAY NOW did not store Ridge"
        );
        std::cout << "library install check: run 0 installed from three registries and played "
                     "Ridge\n";
        return;
    }
    case 1: {
        // 10. ROLL BACK's question over the Library says the game reloads
        // for the mod played; CANCEL changes nothing.
        settle();
        require(
            playing(ridge) && mod_profile() != nullptr && mod_profile()->id == kRidge &&
                revision_in(ridge) == 4,
            "the run does not play Ridge's fourth revision"
        );
        open_the_library();
        show_entry("oa.library.row.check-builtin.ridge");
        press_action("roll-back");
        {
            const auto* asked = dynamic_cast<const QuestionScreen*>(oa_layer().top());
            require(
                asked != nullptr && !asked->question().paragraphs.empty() &&
                    asked->question().paragraphs[0].text.ends_with(
                        "The game reloads its data for the mod and returns to the main menu."
                    ),
                "ROLL BACK's question does not say the game reloads for the mod played"
            );
        }
        tap(SDLK_ESCAPE);
        require(
            top_is(library_screen_name) && !soft_restart_requested() &&
                !install::pending_change_waiting(),
            "CANCEL on ROLL BACK's question changed the mod played"
        );

        // 9. A built-in registry's update to the mod played asks nothing,
        // and waits for the player to leave the mod: no soft restart.
        post_ridge(kBuiltIn, "1.0", 5, 2);
        installs_quietly("check-builtin's update to the mod played", kBuiltIn, kRidge);
        require(
            !soft_restart_requested() && install::pending_change_waiting() &&
                install::pending_change_target() == std::string(kRidge),
            "the update to the mod played did not wait, named, without a soft restart"
        );
        require(
            revision_in(ridge) == 4 &&
                revision_in(
                    mods /
                    (install::folder_names(install::mod_kind()).staging + std::string(kRidge))
                ) == 5,
            "the update to the mod played is not staged whole, the mod left as it is"
        );
        {
            const std::optional<library::QueueItem> item = queued(kBuiltIn, kRidge);
            const library::QueueLines lines = library::queue_lines(host.model);
            require(
                item && item->phase == library::QueueItem::Phase::held &&
                    lines.first == "Waiting · finishes when you leave Ridge",
                "the Library's queue does not say the update waits until Ridge is left"
            );
        }

        // CANCEL on the held line drops it. The catalogue still offers the
        // update, so the Library shows UPDATE again; nothing fetches it until
        // UPDATE is pressed.
        {
            library::UpdateNote offered;
            offered.registry = std::string(kBuiltIn);
            offered.kind = library::Kind::mod;
            offered.key = std::string(kRidge);
            offered.to_release = 2;
            offered.to_version = "1.0";
            offered.to_revision = 5;
            fixture.inputs.updates.push_back(std::move(offered));
            ++fixture.generation;
            frames();
            show_entry("oa.library.row.check-builtin.ridge");
            require(
                named("oa.library.action.cancel").has_value(),
                "the held update's queue line offers no CANCEL"
            );
            press_action("cancel");
            for (int count = 0; count < kMostFrames && state.discarder.busy(); ++count)
                frame();
            frames();
            require(!install::pending_change_waiting(), "CANCEL left the held update waiting");
            require(revision_in(ridge) == 4, "CANCEL changed the mod played");
            require(
                !fs::exists(
                    mods /
                    (install::folder_names(install::mod_kind()).staging + std::string(kRidge))
                ),
                "CANCEL left the staged folder"
            );
            require(!reserved_left(mods), "CANCEL left a folder an install keeps");
            require(!queued(kBuiltIn, kRidge), "CANCEL left the held update's queue line");
            require(
                named("oa.library.action.update").has_value(),
                "the Library does not offer UPDATE again"
            );
            std::cout << "library install check: CANCEL dropped the held update\n";
        }

        // Taken again, and held, so the next turn still finds it made.
        post_ridge(kBuiltIn, "1.0", 5, 2);
        installs_quietly("check-builtin's update taken again", kBuiltIn, kRidge);
        require(
            !soft_restart_requested() && install::pending_change_waiting() &&
                install::pending_change_target() == std::string(kRidge) && revision_in(ridge) == 4,
            "the update taken again did not wait, the mod left as it is"
        );
        {
            const std::optional<library::QueueItem> item = queued(kBuiltIn, kRidge);
            require(
                item && item->phase == library::QueueItem::Phase::held,
                "the update taken again is not held"
            );
        }

        // A second mod, and PLAY NOW on it: switching the mod makes the change.
        const fs::path lattice = packages / "lattice.oamod";
        write_package(lattice, profile_text(kLattice, "Lattice", "3.0", 1), 1);
        post_catalogue(lattice, kBuiltIn, kLattice, "Lattice", 1);
        {
            const uint32_t shown = state.prompts_shown;
            const std::optional<install::PackageOutcome> outcome = until_outcome("Lattice");
            require(
                outcome && outcome->result == install::OutcomeResult::installed &&
                    state.prompts_shown == shown,
                "Lattice was not installed with no prompt"
            );
            take_outcome(*outcome, kBuiltIn, kLattice);
        }
        require(revision_in(ridge) == 4, "the mod played changed before it was left");
        show_entry("oa.library.row.check-builtin.lattice");
        press_action("play-now");
        require(soft_restart_requested(), "PLAY NOW on Lattice did not end the run");
        std::cout << "library install check: run 1 held the update to the mod played\n";
        return;
    }
    case 2: {
        // The update was put in place as the mod was switched, and is told.
        require(playing(mods / std::string(kLattice)), "the run does not play Lattice");
        require(
            revision_in(ridge) == 5 && recorded_from(ridge, kBuiltIn, 2) &&
                revision_in(kept) == 4 && recorded_from(kept, kBuiltIn, 1) &&
                !install::pending_change_waiting(),
            "the held update was not put in place as the mod was switched"
        );
        for (int tick = 0; tick < 30; ++tick)
            tell_mod_installs();
        require(
            state.prompt == nullptr && state.stage != ModInstallState::Stage::telling,
            "the held catalogue update showed a notice"
        );
        // The folders the change dropped go, a step a frame, and nothing an
        // install keeps while it works is left.
        for (int count = 0; count < kMostFrames && state.discarder.busy(); ++count)
            tell_mod_installs();
        require(
            !state.discarder.busy() && !reserved_left(mods),
            "the held update left a folder of its own in Mods"
        );

        // 12. The update question. Ember and Flint come from check-added;
        // Ridge, and Lattice, played, from check-builtin. Ember's update
        // changes the rules and Flint's keeps them; Ridge's changes them
        // and Lattice's cannot be known.
        open_the_library();
        const fs::path ember_file = packages / "ember.oamod";
        write_package(ember_file, profile_text(kEmber, "Ember", "2.0", 1), 1);
        post_catalogue(ember_file, kAdded, kEmber, "Ember", 1);
        installs_quietly("check-added's Ember", kAdded, kEmber);
        const fs::path flint_file = packages / "flint.oamod";
        write_package(flint_file, profile_text(kFlint, "Flint", "1.0", 1), 1);
        post_catalogue(flint_file, kAdded, kFlint, "Flint", 1);
        installs_quietly("check-added's Flint", kAdded, kFlint);
        oa::base::sha256::Digest old_rules{};
        old_rules[0] = 0x9c;
        oa::base::sha256::Digest new_rules{};
        new_rules[0] = 0x51;
        const auto fixture_update = [&](std::string_view registry,
                                        std::string_view key,
                                        std::string_view version,
                                        int64_t revision,
                                        library::UpdateNote::Rules rules) {
            library::UpdateNote update;
            update.registry = std::string(registry);
            update.kind = library::Kind::mod;
            update.key = std::string(key);
            update.to_release = 9;
            update.to_version = std::string(version);
            update.to_revision = revision;
            update.size = 3355443;
            update.rules = rules;
            update.from_rules = old_rules;
            if (rules == library::UpdateNote::Rules::changes)
                update.to_rules = new_rules;
            else if (rules == library::UpdateNote::Rules::same)
                update.to_rules = old_rules;
            fixture.inputs.updates.push_back(std::move(update));
        };
        fixture.queued.clear();
        fixture.inputs.queue.clear();
        fixture.inputs.updates.clear();
        fixture_update(kAdded, kEmber, "2.0", 2, library::UpdateNote::Rules::changes);
        fixture_update(kAdded, kFlint, "1.0", 2, library::UpdateNote::Rules::same);
        fixture_update(kBuiltIn, kRidge, "1.0", 6, library::UpdateNote::Rules::changes);
        fixture_update(kBuiltIn, kLattice, "3.0", 2, library::UpdateNote::Rules::unknown);
        list_installed();
        frames();
        const auto queued_now = [&](std::string_view registry, std::string_view key) {
            return !fixture.queued.empty() && fixture.queued.back().registry == registry &&
                   fixture.queued.back().key == key &&
                   fixture.queued.back().reason == content::DownloadReason::update;
        };
        const auto asked = [&]() -> const QuestionScreen* {
            return dynamic_cast<const QuestionScreen*>(oa_layer().top());
        };
        const auto asks_update = [&](std::string_view title, std::string_view over) {
            const QuestionScreen* question = asked();
            require(
                question != nullptr && question->question().title == title &&
                    oa_layer().find(over) != nullptr,
                std::string("the update question ") + std::string(title) + " was not asked over " +
                    std::string(over)
            );
            const settings::Prompt& shown = question->question();
            require(
                shown.buttons.size() == 2 && shown.buttons[0].id == library::not_now_button_id &&
                    shown.buttons[1].id == library::update_button_id && shown.buttons[1].accent &&
                    shown.marked == 0,
                "the update question is not NOT NOW, marked, then UPDATE, primary"
            );
            frame();
            require(
                named("oa.prompt.not-now").has_value() && named("oa.prompt.update").has_value() &&
                    named("oa.prompt.body").has_value(),
                "automation does not list the update question's oa.prompt.not-now, update and body"
            );
        };

        // From the Library: Ember asks first, NOT NOW leaving it and UPDATE
        // queuing it; Flint is queued at once.
        show_entry("oa.library.row.check-added.ember");
        press_action("update");
        asks_update("Update Ember", library_screen_name);
        {
            const settings::Prompt& shown = asked()->question();
            require(
                shown.paragraphs.size() == 4 &&
                    shown.paragraphs[0].text == "Ember 2.0 · revision 1 → 2" &&
                    shown.paragraphs[1].text.starts_with(
                        "Revision 2 changes the game's rules. Saved games from revision 1 won't "
                        "load under revision 2."
                    ) &&
                    shown.paragraphs[2].text == "Rules: 9c00 0000 → 5100 0000" &&
                    shown.paragraphs[3].text == "Download: 3.2 MB",
                "the update question does not say what Ember's update does"
            );
        }
        tap(SDLK_ESCAPE);
        require(
            top_is(library_screen_name) && fixture.queued.empty(),
            "NOT NOW on the update question queued Ember's update"
        );
        press_action("update");
        asks_update("Update Ember", library_screen_name);
        click_named("oa.prompt.update");
        require(
            top_is(library_screen_name) && fixture.queued.size() == 1 && queued_now(kAdded, kEmber),
            "UPDATE on the update question did not queue Ember's update"
        );
        show_entry("oa.library.row.check-added.flint");
        press_action("update");
        require(
            top_is(library_screen_name) && fixture.queued.size() == 2 && queued_now(kAdded, kFlint),
            "an added registry's update that keeps the rules was not queued at once"
        );

        // The built-in registry's: their Rules facts say what they do, and
        // UPDATE queues them at once with no question.
        const auto rules_say = [&](std::string_view part) {
            const std::optional<AutomationControl> rules = named("oa.library.rules");
            return rules.has_value() && rules->text.find(part) != std::string::npos;
        };
        show_entry("oa.library.row.check-builtin.ridge");
        require(
            rules_say("changes the game's rules"),
            "the Library's Rules fact does not say Ridge's update changes the game's rules"
        );
        press_action("update");
        require(
            top_is(library_screen_name) && fixture.queued.size() == 3 &&
                queued_now(kBuiltIn, kRidge),
            "the built-in registry's update that changes the rules asked or was not queued"
        );
        show_entry("oa.library.row.check-builtin.lattice");
        require(
            rules_say("unknown"),
            "the Library's Rules fact does not say Lattice's rules are unknown"
        );
        press_action("update");
        require(
            top_is(library_screen_name) && fixture.queued.size() == 4 &&
                queued_now(kBuiltIn, kLattice),
            "the built-in registry's update with unknown rules asked or was not queued"
        );

        // Again, each in turn: each
        // entry's status line and Rules fact say what its update does, CHECK
        // NOW can ask the registries, the built-in registry's updates are
        // queued at once, and Ember asks over the Library.
        const auto status_says = [&](std::string_view part) {
            const std::optional<AutomationControl> status = named("oa.library.status");
            return status.has_value() && status->text.find(part) != std::string::npos;
        };
        show_entry("oa.library.row.check-builtin.ridge");
        require(
            status_says("Revision 6 available · 3.2 MB") && rules_say("changes the game's rules"),
            "the Library does not say what Ridge's update does"
        );
        show_entry("oa.library.row.check-builtin.lattice");
        require(
            status_says("Revision 2 available · 3.2 MB") && rules_say("unknown"),
            "the Library does not say what Lattice's update does"
        );
        show_entry("oa.library.row.check-added.ember");
        require(
            status_says("Revision 2 available · 3.2 MB") && rules_say("changes the game's rules"),
            "the Library does not say what Ember's update does"
        );
        {
            // The refresh icon is in the header. Compact's details page goes
            // back to the list first. It stays disabled, and says so, while
            // the registries are being asked.
            if (named("oa.library.back"))
                click_named("oa.library.back");
            const std::optional<AutomationControl> off = named("oa.library.refresh");
            require(
                off.has_value() && !off->enabled && off->text == "Refresh",
                "the Library offers refresh with no registry to ask"
            );
            fixture.inputs.can_check = true;
            ++fixture.generation;
            frames();
            const std::optional<AutomationControl> on = named("oa.library.refresh");
            require(
                on.has_value() && on->enabled && on->text == "Refresh",
                "the Library offers no refresh icon to ask the registries"
            );
            fixture.inputs.checking = true;
            ++fixture.generation;
            frames();
            const std::optional<AutomationControl> spinning = named("oa.library.refresh");
            require(
                spinning.has_value() && !spinning->enabled && spinning->text == "Refreshing",
                "the refresh icon can be pressed, or does not say it is refreshing, while a "
                "registry is being asked"
            );
            fixture.inputs.checking = false;
            fixture.inputs.can_check = false;
            ++fixture.generation;
            frames();
        }
        show_entry("oa.library.row.check-builtin.ridge");
        press_action("update");
        require(
            top_is(library_screen_name) && fixture.queued.size() == 5 &&
                queued_now(kBuiltIn, kRidge),
            "UPDATE on Ridge's entry asked or did not queue its update"
        );
        show_entry("oa.library.row.check-builtin.lattice");
        press_action("update");
        require(
            top_is(library_screen_name) && fixture.queued.size() == 6 &&
                queued_now(kBuiltIn, kLattice),
            "UPDATE on Lattice's entry asked or did not queue its update"
        );
        show_entry("oa.library.row.check-added.ember");
        press_action("update");
        asks_update("Update Ember", library_screen_name);
        tap(SDLK_ESCAPE);
        require(
            top_is(library_screen_name) && fixture.queued.size() == 6,
            "NOT NOW on the update question over the Library queued Ember's update"
        );
        std::cout << "library install check: built-in update queued with no question\n";

        // UPDATE ALL: the built-in registry's updates, and the added one's
        // that keeps the rules, at once in list order; one question about
        // Ember alone, NOT NOW leaving it.
        close_the_library();
        fixture.queued.clear();
        list_installed();
        open_library(library::Tab::updates);
        frames();
        require(top_is(library_screen_name), "the Library did not open on Updates");
        click_named("oa.library.action.update-all");
        {
            const QuestionScreen* question = asked();
            require(
                question != nullptr && question->question().title == "Update all" &&
                    question->question().paragraphs.size() == 1 &&
                    question->question().paragraphs[0].text.starts_with(
                        "1 update changes the game's rules: Ember."
                    ),
                "UPDATE ALL did not ask about Ember alone"
            );
            bool ember = false;
            bool ridge_queued = false;
            bool lattice_queued = false;
            for (const FeedQueued& each : fixture.queued) {
                ember = ember || each.key == kEmber;
                ridge_queued = ridge_queued || (each.registry == kBuiltIn && each.key == kRidge);
                lattice_queued =
                    lattice_queued || (each.registry == kBuiltIn && each.key == kLattice);
            }
            require(
                !ember && ridge_queued && lattice_queued && fixture.queued.size() == 3,
                "UPDATE ALL did not queue the built-in registry's updates and Flint's at once"
            );
        }
        tap(SDLK_ESCAPE);
        require(
            top_is(library_screen_name) && fixture.queued.size() == 3,
            "NOT NOW on UPDATE ALL's question queued Ember's update"
        );
        click_named("oa.library.action.update-all");
        click_named("oa.prompt.update");
        require(
            fixture.queued.size() == 7 && queued_now(kAdded, kEmber),
            "UPDATE on UPDATE ALL's question did not queue Ember's update"
        );
        fixture.inputs.updates.clear();
        ++fixture.generation;
        close_the_library();

        // UNINSTALL. A map pack's is off while a game is played, and says
        // why; the language pack in use and the mod being played, Lattice,
        // show a notice and keep their folders; a built-in registry's mod
        // asks first, CANCEL removing nothing and UNINSTALL its folder and
        // the copy ROLL BACK keeps, and its row stays, not installed; and a
        // mod of the player's own leaves the Library with its folder.
        std::error_code uninstall_error;
        const fs::path maps_folder = user_folder_ / "Maps";
        const fs::path languages_folder = user_folder_ / "Languages";
        const fs::path pack_folder = maps_folder / std::string(kArchipelago);
        const fs::path language_folder = languages_folder / std::string(kPseudo);
        for (const fs::path& made : {pack_folder, language_folder}) {
            fs::create_directories(made, uninstall_error);
            std::ofstream(made / "kept.txt") << "kept";
        }
        const auto still_there = [](const fs::path& made) { return fs::exists(made / "kept.txt"); };
        const auto beside = [&](library::Kind kind,
                                std::string_view key,
                                std::string_view name,
                                const fs::path& folder) {
            library::Installed shown;
            shown.kind = kind;
            shown.key = std::string(key);
            shown.folder = folder;
            shown.name = std::string(name);
            shown.version = "1.0";
            shown.registry = std::string(kBuiltIn);
            shown.release = 1;
            return shown;
        };
        beside_mods.push_back(
            beside(library::Kind::map_pack, kArchipelago, "Archipelago", pack_folder)
        );
        beside_mods.push_back(beside(library::Kind::language, kPseudo, "Pseudo", language_folder));
        beside_mods.back().in_use = true;
        const auto notice_says = [&](std::string_view title, std::string_view text) {
            const QuestionScreen* told = asked();
            return told != nullptr && told->question().title == title &&
                   told->question().paragraphs.size() == 1 &&
                   told->question().paragraphs[0].text == text &&
                   told->question().buttons.size() == 1 &&
                   told->question().buttons[0].id == library::uninstall_ok_button_id;
        };
        open_the_library();

        // A map pack while a game is played: UNINSTALL is off and the note
        // says why; the runtime, asked directly, does nothing either.
        fixture.inputs.in_match = true;
        ++fixture.generation;
        frames();
        click_named("oa.library.tab.maps");
        frames();
        show_entry("oa.library.row.check-builtin.archipelago");
        {
            const std::optional<AutomationControl> button = named("oa.library.action.uninstall");
            const std::optional<AutomationControl> note = named("oa.library.note");
            require(
                button.has_value() && !button->enabled && note.has_value() &&
                    note->text == "Map packs can't be uninstalled during a game.",
                "a map pack's UNINSTALL is not off during a game, with its reason"
            );
        }
        library_entry_action(
            library::Action::uninstall,
            {std::string(kBuiltIn), library::Kind::map_pack, std::string(kArchipelago)}
        );
        frame();
        require(
            top_is(library_screen_name) && still_there(pack_folder),
            "UNINSTALL of a map pack during a game did something"
        );
        fixture.inputs.in_match = false;
        ++fixture.generation;
        frames();

        // The language pack in use: a notice, and its folder stays.
        click_named("oa.library.tab.languages");
        frames();
        show_entry("oa.library.row.check-builtin.en-xa");
        press_action("uninstall");
        require(
            notice_says(
                "Uninstall Pseudo",
                "The game is shown in Pseudo. Choose another language in Settings › Language "
                "first, then uninstall it."
            ),
            "UNINSTALL of the language pack in use did not say to choose another language first"
        );
        frame();
        require(
            named("oa.prompt.ok").has_value(), "automation does not list the notice's oa.prompt.ok"
        );
        tap(SDLK_RETURN);
        require(
            top_is(library_screen_name) && still_there(language_folder),
            "the notice for the language pack in use did not close, or its folder went"
        );
        click_named("oa.library.tab.mods");
        frames();

        // The mod being played: a notice, and its folder stays.
        show_entry("oa.library.row.check-builtin.lattice");
        press_action("uninstall");
        require(
            notice_says(
                "Uninstall Lattice",
                "You are playing Lattice. Switch it off first with SWITCH OFF, then uninstall it."
            ),
            "UNINSTALL of the mod being played did not say to switch it off first"
        );
        tap(SDLK_RETURN);
        require(
            top_is(library_screen_name) && revision_in(mods / std::string(kLattice)) == 1,
            "the notice for the mod being played did not close, or the mod changed"
        );

        // A built-in registry's mod with a copy kept: the question, CANCEL
        // removing nothing, and UNINSTALL removing both.
        const fs::path basalt = mods / std::string(kBasalt);
        for (const int release : {1, 2}) {
            const fs::path file = packages / ("basalt-" + std::to_string(release) + ".oamod");
            write_package(file, profile_text(kBasalt, "Basalt", "1.0", release), release);
            post_catalogue(file, kBuiltIn, kBasalt, "Basalt", release);
            installs_quietly("check-builtin's Basalt", kBuiltIn, kBasalt);
        }
        require(
            revision_in(basalt) == 2 &&
                revision_in(basalt / std::string(install::backup_folder_name)) == 1,
            "Basalt's second release did not keep the first"
        );
        {
            library::Listing listed;
            listed.registry = std::string(kBuiltIn);
            listed.kind = library::Kind::mod;
            listed.key = std::string(kBasalt);
            listed.name = "Basalt";
            listed.version = "1.0";
            listed.revision = 2;
            listed.release = 2;
            listed.size = 1024;
            fixture.inputs.listings.push_back(std::move(listed));
            ++fixture.generation;
            frames();
        }
        const auto asks_uninstall = [&](std::string_view title,
                                        std::string_view first,
                                        std::string_view second) {
            const QuestionScreen* question = asked();
            require(
                question != nullptr && question->question().title == title &&
                    oa_layer().find(library_screen_name) != nullptr,
                std::string("UNINSTALL did not ask ") + std::string(title) + " over the Library"
            );
            const settings::Prompt& shown = question->question();
            require(
                shown.paragraphs.size() == 2 && shown.paragraphs[0].text == first &&
                    shown.paragraphs[1].text == second,
                std::string(title) + " does not say what it removes"
            );
            require(
                shown.buttons.size() == 2 &&
                    shown.buttons[0].id == library::uninstall_cancel_button_id &&
                    shown.buttons[1].id == library::uninstall_button_id &&
                    shown.buttons[1].accent && shown.marked == 0,
                std::string(title) + " is not CANCEL, marked, then UNINSTALL, primary"
            );
            frame();
            require(
                named("oa.prompt.cancel").has_value() && named("oa.prompt.uninstall").has_value() &&
                    named("oa.prompt.body").has_value(),
                "automation does not list the question's oa.prompt.cancel, uninstall and body"
            );
        };
        const auto drain = [&] {
            for (int count = 0; count < kMostFrames && state.discarder.busy(); ++count)
                frame();
            require(
                !state.discarder.busy() && !reserved_left(mods),
                "an uninstall left a folder of its own in Mods"
            );
        };
        show_entry("oa.library.row.check-builtin.basalt");
        press_action("uninstall");
        asks_uninstall(
            "Uninstall Basalt",
            "Uninstall Basalt? Its folder, Mods/basalt, is removed, and with it the copy ROLL BACK "
            "keeps.",
            "GET installs it again."
        );
        tap(SDLK_ESCAPE);
        require(
            top_is(library_screen_name) && revision_in(basalt) == 2 &&
                revision_in(basalt / std::string(install::backup_folder_name)) == 1,
            "CANCEL on the Uninstall question changed Basalt"
        );
        press_action("uninstall");
        click_named("oa.prompt.uninstall");
        require(
            top_is(library_screen_name) && !fs::exists(fs::symlink_status(basalt)),
            "UNINSTALL did not remove Basalt's folder"
        );
        drain();
        list_installed();
        frames();
        {
            const library::Entry* entry = library::find_entry(
                host.model, {std::string(kBuiltIn), library::Kind::mod, std::string(kBasalt)}
            );
            require(
                entry != nullptr && entry->installed == nullptr &&
                    entry->state == library::State::get &&
                    named("oa.library.row.check-builtin.basalt").has_value(),
                "the Library does not show Basalt as not installed"
            );
        }
        std::cout << "library install check: UNINSTALL removed a built-in registry's mod\n";

        // A mod of the player's own, which no registry lists, leaves the Library.
        const fs::path scree = mods / std::string(kScree);
        fs::create_directories(scree, uninstall_error);
        std::ofstream(scree / "oamod.yaml") << profile_text(kScree, "Scree", "1.0", 1);
        list_installed();
        frames();
        show_entry("oa.library.row.scree");
        press_action("uninstall");
        asks_uninstall(
            "Uninstall Scree",
            "Uninstall Scree? Its folder, Mods/scree, is removed.",
            "No registry lists it, so the Library cannot get it again."
        );
        click_named("oa.prompt.uninstall");
        require(
            top_is(library_screen_name) && !fs::exists(fs::symlink_status(scree)),
            "UNINSTALL did not remove the player's own mod"
        );
        drain();
        list_installed();
        frames();
        require(
            library::find_entry(
                host.model, {std::string(), library::Kind::mod, std::string(kScree)}
            ) == nullptr &&
                !named("oa.library.row.scree").has_value(),
            "the player's own mod is still in the Library"
        );
        require(
            still_there(pack_folder) && still_there(language_folder) &&
                revision_in(mods / std::string(kLattice)) == 1 && revision_in(ridge) == 5,
            "an uninstall removed something it was not asked to"
        );
        beside_mods.clear();
        list_installed();
        close_the_library();
        fs::remove_all(maps_folder, uninstall_error);
        fs::remove_all(languages_folder, uninstall_error);
        std::cout
            << "library install check: UNINSTALL kept the mod played and the language in use\n";

        // The last run plays No Mod, as the next start of the check does.
        preference_values_.erase(std::string(settings::key::mod_directory));
        preferences_dirty_ = true;
        request_soft_restart();
        std::cout << "library install check: run 2 found the held update made and queued the "
                     "updates\n";
        return;
    }
    case 3: {
        // 12. No Mod is played. GET WITH on Gatehouse Keep asks, CANCEL queues
        // nothing and GET BOTH queues Gatehouse then Gatehouse Keep. With both
        // installed, uninstalling Gatehouse says Gatehouse Keep needs it. Those
        // folders are removed. GET on Fast Builders then queues it, the package
        // installs under Mutators with its origin record, CHECK WITH THIS MOD
        // runs the fixture's fake checker, OPEN FOLDER and HOMEPAGE reach the
        // recorders, and UNINSTALL asks and removes the folder. Ridge stays
        // as the previous run left it.
        settle();
        require(options_.layered_mod_folder.empty(), "the mutator run plays a mod");
        require(revision_in(ridge) == 5, "the mutator run does not find Ridge's fifth revision");
        const fs::path mutators_root =
            user_folder_ / std::string(install::oamut::mutators_folder_name);
        const fs::path fast_folder = mutators_root / std::string(kFast);
        {
            library::Listing gatehouse;
            gatehouse.registry = std::string(kBuiltIn);
            gatehouse.kind = library::Kind::mutator;
            gatehouse.key = std::string(kGatehouse);
            gatehouse.name = "Gatehouse";
            gatehouse.version = "4.1";
            gatehouse.release = 1;
            gatehouse.size = 1024;
            library::MutatorFacts gate_facts;
            gate_facts.tags.push_back("gameplay");
            gatehouse.mutator = std::move(gate_facts);
            library::Listing keep;
            keep.registry = std::string(kBuiltIn);
            keep.kind = library::Kind::mutator;
            keep.key = std::string(kGatehouseKeep);
            keep.name = "Gatehouse Keep";
            keep.version = "2.0";
            keep.release = 1;
            keep.size = 1024;
            library::MutatorFacts keep_facts;
            keep_facts.tags.push_back("gameplay");
            keep_facts.needs.push_back({"gatehouse", ">= 4.0", "Gatehouse"});
            keep.mutator = std::move(keep_facts);
            fixture.inputs.listings.push_back(std::move(gatehouse));
            fixture.inputs.listings.push_back(std::move(keep));
            ++fixture.generation;
            list_installed();
            open_library(library::Tab::mutators);
            frames();
            require(top_is(library_screen_name), "the Library did not open for GET WITH");
            show_entry("oa.library.row.check-builtin.gatehouse-keep");
            const std::size_t queued_before = fixture.queued.size();
            press_action("get-with");
            {
                const auto* asked = dynamic_cast<const QuestionScreen*>(oa_layer().top());
                require(
                    asked != nullptr && oa_layer().find(library_screen_name) != nullptr,
                    "GET WITH did not ask over the Library"
                );
                const settings::Prompt& shown = asked->question();
                require(shown.title == "Get Gatehouse Keep?", "GET WITH's title is wrong");
                require(
                    shown.paragraphs.size() == 1 &&
                        shown.paragraphs[0].text ==
                            "Gatehouse Keep needs Gatehouse 4.0 or later, which you don't have. "
                            "Both download now, and Gatehouse goes above Gatehouse Keep in every "
                            "list.",
                    "GET WITH's paragraph is wrong"
                );
                require(
                    shown.items.size() == 2 && shown.items[0].text == "Gatehouse 4.1" &&
                        shown.items[0].aside == " · from Check Built-in" &&
                        shown.items[1].text == "Gatehouse Keep 2.0" &&
                        shown.items[1].aside == " · from Check Built-in",
                    "GET WITH does not list both packages"
                );
                require(
                    shown.buttons.size() == 2 && shown.buttons[0].id == "cancel" &&
                        shown.buttons[1].id == "get-both" && shown.buttons[1].accent &&
                        shown.marked == 0,
                    "GET WITH is not CANCEL, marked, then GET BOTH"
                );
            }
            require(
                named("oa.prompt.get-both").has_value() && named("oa.prompt.cancel").has_value(),
                "automation does not list oa.prompt.get-both"
            );
            click_named("oa.prompt.cancel");
            frames();
            require(
                top_is(library_screen_name) && fixture.queued.size() == queued_before,
                "CANCEL on GET WITH queued a package"
            );
            press_action("get-with");
            click_named("oa.prompt.get-both");
            frames();
            require(
                top_is(library_screen_name) && fixture.queued.size() == queued_before + 2 &&
                    fixture.queued[queued_before].registry == kBuiltIn &&
                    fixture.queued[queued_before].key == kGatehouse &&
                    fixture.queued[queued_before].reason == content::DownloadReason::install &&
                    fixture.queued[queued_before + 1].registry == kBuiltIn &&
                    fixture.queued[queued_before + 1].key == kGatehouseKeep &&
                    fixture.queued[queued_before + 1].reason == content::DownloadReason::install,
                "GET BOTH did not queue Gatehouse then Gatehouse Keep"
            );
            std::cout << "library install check: GET BOTH queued gatehouse then gatehouse-keep\n";

            const fs::path gate_folder = mutators_root / std::string(kGatehouse);
            const fs::path keep_folder = mutators_root / std::string(kGatehouseKeep);
            write_named_mutator(packages / "gatehouse.oamut", kGatehouse, "Gatehouse", "4.1", {});
            post_catalogue(
                packages / "gatehouse.oamut",
                kBuiltIn,
                kGatehouse,
                "Gatehouse",
                1,
                library::Kind::mutator
            );
            installs_quietly("Gatehouse", kBuiltIn, kGatehouse);
            write_named_mutator(
                packages / "gatehouse-keep.oamut",
                kGatehouseKeep,
                "Gatehouse Keep",
                "2.0",
                "requires:\n  mutators: [\"gatehouse >= 4.0\"]\n"
            );
            post_catalogue(
                packages / "gatehouse-keep.oamut",
                kBuiltIn,
                kGatehouseKeep,
                "Gatehouse Keep",
                1,
                library::Kind::mutator
            );
            installs_quietly("Gatehouse Keep", kBuiltIn, kGatehouseKeep);
            show_entry("oa.library.row.check-builtin.gatehouse");
            press_action("uninstall");
            {
                const auto* asked = dynamic_cast<const QuestionScreen*>(oa_layer().top());
                require(asked != nullptr, "UNINSTALL did not ask about Gatehouse");
                const settings::Prompt& shown = asked->question();
                require(
                    shown.paragraphs.size() == 3 &&
                        shown.paragraphs[0].text ==
                            "Uninstall Gatehouse? Its folder, Mutators/gatehouse, is removed." &&
                        shown.paragraphs[1].text ==
                            "Gatehouse Keep needs it: it stays installed, and cannot be switched "
                            "on until this is back." &&
                        shown.paragraphs[2].text == "GET installs it again.",
                    "uninstalling Gatehouse did not say Gatehouse Keep needs it"
                );
            }
            click_named("oa.prompt.cancel");
            frames();
            require(fs::exists(gate_folder) && fs::exists(keep_folder), "CANCEL removed Gatehouse");
            close_the_library();
            std::error_code gate_error;
            fs::remove_all(gate_folder, gate_error);
            fs::remove_all(keep_folder, gate_error);
            std::erase_if(fixture.inputs.listings, [](const library::Listing& listed) {
                return listed.kind == library::Kind::mutator &&
                       (listed.key == kGatehouse || listed.key == kGatehouseKeep);
            });
            list_installed();
        }
        std::error_code fast_error;
        fs::remove_all(fast_folder, fast_error);

        {
            library::Listing listed;
            listed.registry = std::string(kBuiltIn);
            listed.kind = library::Kind::mutator;
            listed.key = std::string(kFast);
            listed.name = "Fast Builders";
            listed.version = "2.0";
            listed.revision = 1;
            listed.release = 1;
            listed.author = "Ada";
            listed.summary = "Builds faster.";
            listed.homepage = "https://example.test/fast-builders";
            listed.size = 1024;
            library::MutatorFacts facts;
            facts.works_with.push_back({"*", {}, {}});
            facts.layers.push_back("units");
            facts.tags.push_back("economy");
            listed.mutator = std::move(facts);
            fixture.inputs.listings.push_back(std::move(listed));
            ++fixture.generation;
        }
        list_installed();
        open_library(library::Tab::mutators);
        frames();
        require(top_is(library_screen_name), "the Library did not open on Mutators");
        show_entry("oa.library.row.check-builtin.fast-builders");
        const std::size_t queued_before = fixture.queued.size();
        press_action("get");
        require(
            fixture.queued.size() == queued_before + 1 &&
                fixture.queued.back().registry == kBuiltIn && fixture.queued.back().key == kFast &&
                fixture.queued.back().reason == content::DownloadReason::install,
            "GET did not queue Fast Builders"
        );
        std::cout << "library install check: mutator queued\n";

        const fs::path fast_file = packages / "fast-builders.oamut";
        write_mutator(fast_file);
        post_catalogue(fast_file, kBuiltIn, kFast, "Fast Builders", 1, library::Kind::mutator);
        installs_quietly("Fast Builders", kBuiltIn, kFast);
        {
            const std::optional<install::Origin> record = install::read_origin(fast_folder);
            require(
                fs::exists(fast_folder) && record &&
                    record->kind == install::OriginKind::catalogue &&
                    record->registry == kBuiltIn && record->catalogue_id == kFast &&
                    record->release == 1,
                "Fast Builders is not under Mutators with its origin record"
            );
        }
        std::cout << "library install check: mutator installed\n";

        // An opened .oamut takes the same path as an opened .oamod: the system hands it to
        // the game as a dropped file, as the Finder, the Files app and a drag onto the window
        // do, which the game knows by its extension and posts to the inbox; the inbox asks,
        // INSTALL puts it in Mutators under its id, whatever the file is called, with a file
        // origin, and the Library lists the row. Removed again so the Fast Builders checks
        // below still see that row alone.
        close_the_library();
        const fs::path opened_mutator = packages / "Steady Hands 1.0.OAMUT";
        write_named_mutator(opened_mutator, "steady-hands", "Steady Hands", "1.0", {});
        require(install::opens_file(opened_mutator), "the game does not open a .oamut file");
        static std::string dropped_mutator;
        dropped_mutator = path_to_utf8(opened_mutator);
        SDL_Event drop{};
        drop.type = SDL_EVENT_DROP_FILE;
        drop.drop.windowID = SDL_GetWindowID(sdl_.window);
        drop.drop.data = dropped_mutator.c_str();
        require(SDL_PushEvent(&drop), "the opened mutator's drop could not be sent");
        until_prompt("an opened mutator");
        require(
            says("Install the mutator Steady Hands 1.0?"),
            "an opened mutator did not name itself in its question"
        );
        require(titled("INSTALL MUTATOR"), "an opened mutator did not ask INSTALL MUTATOR");
        tap(SDLK_RETURN);
        until_prompt("the opened mutator's notice");
        require(titled("MUTATOR INSTALLED"), "an opened mutator was not installed");
        tap(SDLK_RETURN);
        require(state.prompt == nullptr, "Enter did not close the opened mutator's notice");
        const fs::path steady_folder =
            user_folder_ / std::string(install::oamut::mutators_folder_name) / "steady-hands";
        {
            const std::optional<install::Origin> record = install::read_origin(steady_folder);
            require(
                fs::is_regular_file(steady_folder / "oamut.yaml") && record &&
                    record->kind == install::OriginKind::file &&
                    record->sha256 == file_hash(opened_mutator),
                "the opened mutator is not under Mutators with a file origin"
            );
        }
        list_installed();
        open_library(library::Tab::mutators);
        frames();
        require(
            library::find_entry(
                host.model, {std::string(), library::Kind::mutator, std::string("steady-hands")}
            ) != nullptr,
            "the Library does not list the opened mutator"
        );
        std::cout << "library install check: opened mutator installed\n";
        close_the_library();
        std::error_code steady_error;
        fs::remove_all(steady_folder, steady_error);
        require(!steady_error, "the opened mutator's folder could not be removed");
        list_installed();
        open_library(library::Tab::mutators);
        frames();
        show_entry("oa.library.row.check-builtin.fast-builders");

        host.feed.remembered_verdict = feed_remembered;
        host.feed.start_check = feed_start;
        host.feed.keep_verdict = feed_keep;
        ++fixture.generation;
        frames();

        const auto fast_entry = [&]() -> const library::Entry* {
            return library::find_entry(
                host.model, {std::string(kBuiltIn), library::Kind::mutator, std::string(kFast)}
            );
        };
        const auto checked_value = [&]() -> std::string {
            const library::Entry* shown_entry = fast_entry();
            if (shown_entry == nullptr)
                return {};
            for (const library::Fact& fact : library::facts(host.model, *shown_entry))
                if (fact.long_label == library::words::fact_checked)
                    return fact.long_value;
            return {};
        };
        const auto offers_check = [&]() -> bool {
            const library::Entry* shown_entry = fast_entry();
            if (shown_entry == nullptr)
                return false;
            for (const library::ActionButton& button :
                 library::entry_actions(host.model, *shown_entry))
                if (button.action == library::Action::check_with_mod)
                    return true;
            return false;
        };
        {
            const library::Entry* shown_entry = fast_entry();
            require(
                shown_entry != nullptr && shown_entry->installed != nullptr && offers_check() &&
                    library::status_line(host.model, *shown_entry) ==
                        library::words::status_installed,
                "CHECK WITH THIS MOD is not offered for the installed mutator"
            );
        }

        const auto wait_until = [&](const auto& ready, std::string_view what) {
            for (int count = 0; count < kMostFrames && !ready(); ++count)
                frame();
            require(ready(), what);
        };

        fixture.release.store(false);
        fixture.work_started.store(false);
        fixture.canned = {};
        fixture.canned.verdict = oa::data::mutators::Verdict::compatible;
        fixture.canned.counts.units_changed = 2u;
        press_action("check-with-mod");
        wait_until(
            [&] { return fixture.work_started.load(); }, "the check did not start on the worker"
        );
        require(
            checked_value().find("Checking with No Mod") != std::string::npos,
            "the Checked fact did not say the check was running"
        );
        fixture.release.store(true);
        wait_until([&] { return fixture.keep_count >= 1; }, "keep_verdict was not called");
        require(
            checked_value().find("changes 2 units") != std::string::npos,
            "the Checked fact does not say changes 2 units"
        );
        std::cout << "library install check: mutator checked (fake verdict)\n";

        fixture.release.store(false);
        fixture.work_started.store(false);
        fixture.canned = {};
        fixture.canned.verdict = oa::data::mutators::Verdict::incompatible;
        fixture.canned.reason = "needs a unit";
        press_action("check-with-mod");
        wait_until([&] { return fixture.work_started.load(); }, "the second check did not start");
        fixture.release.store(true);
        wait_until([&] { return fixture.keep_count >= 2; }, "the second verdict was not kept");
        {
            const library::Entry* shown_entry = fast_entry();
            require(
                shown_entry != nullptr && shown_entry->not_compatible &&
                    checked_value().find("needs a unit") != std::string::npos,
                "an incompatible verdict did not mark the row"
            );
        }

        host.feed.remembered_verdict = nullptr;
        host.feed.start_check = nullptr;
        host.feed.keep_verdict = nullptr;
        ++fixture.generation;
        frames();
        require(
            checked_value().empty() && !offers_check(),
            "null verdict entries still show Checked or CHECK WITH THIS MOD"
        );

        require(web_links_ != nullptr, "the run keeps no record of web links");
        const std::size_t opened = user_folder_state().opened.size();
        press_action("open-folder");
        require(
            user_folder_state().opened.size() == opened + 1 &&
                kept_path(user_folder_state().opened.back()) == kept_path(fast_folder),
            "OPEN FOLDER did not show Fast Builders' folder"
        );
        const std::size_t links = web_links_->requests.size();
        press_action("homepage");
        require(
            web_links_->requests.size() == links + 1 &&
                web_links_->requests.back() == "https://example.test/fast-builders",
            "HOMEPAGE did not record Fast Builders' homepage"
        );

        press_action("uninstall");
        {
            const auto* asked = dynamic_cast<const QuestionScreen*>(oa_layer().top());
            require(
                asked != nullptr && oa_layer().find(library_screen_name) != nullptr,
                "UNINSTALL did not ask over the Library"
            );
            const settings::Prompt& shown = asked->question();
            require(shown.title == "Uninstall Fast Builders", "the question's title is wrong");
            require(
                shown.paragraphs.size() == 2 &&
                    shown.paragraphs[0].text == "Uninstall Fast Builders? Its folder, "
                                                "Mutators/fast-builders, is removed." &&
                    shown.paragraphs[1].text == "GET installs it again.",
                "the question does not say what it removes"
            );
            require(
                shown.buttons.size() == 2 &&
                    shown.buttons[0].id == library::uninstall_cancel_button_id &&
                    shown.buttons[1].id == library::uninstall_button_id &&
                    shown.buttons[1].accent && shown.marked == 0,
                "the question is not CANCEL, marked, then UNINSTALL, primary"
            );
        }
        frame();
        require(
            named("oa.prompt.uninstall").has_value(), "automation does not list oa.prompt.uninstall"
        );
        click_named("oa.prompt.uninstall");
        for (int count = 0; count < kMostFrames &&
                            (state.discarder.busy() || fs::exists(fs::symlink_status(fast_folder)));
             ++count)
            frame();
        require(
            !fs::exists(fs::symlink_status(fast_folder)),
            "UNINSTALL did not remove Fast Builders' folder"
        );
        list_installed();
        frames();
        {
            const library::Entry* shown_entry = fast_entry();
            require(
                shown_entry != nullptr && shown_entry->installed == nullptr &&
                    shown_entry->state == library::State::get,
                "Fast Builders is not shown as not installed"
            );
        }
        std::cout << "library install check: mutator uninstalled\n";

        // The fixture's checker is done. The runtime's own verdict entries
        // stay on this feed, whose queue and inputs are still the fixture's,
        // and two made-up packages are dry-run against the installed game.
        // The fixture stays the feed's context. These call the runtime's own
        // entries with that fixture's runtime, and they are written here so
        // the private host is in reach.
        host.feed.remembered_verdict = [](void* context, std::string_view id) {
            return LibraryHost::own_remembered_verdict(
                static_cast<InstallFeed*>(context)->runtime, id
            );
        };
        host.feed.start_check = [](void* context, std::string_view id) {
            return LibraryHost::own_start_check(static_cast<InstallFeed*>(context)->runtime, id);
        };
        host.feed.keep_verdict = [](void* context,
                                    std::string_view id,
                                    const oa::data::mutators::VerdictRecord& record) {
            LibraryHost::own_keep_verdict(static_cast<InstallFeed*>(context)->runtime, id, record);
        };
        ++fixture.generation;
        frames();

        const auto entry_for = [&](std::string_view key) -> const library::Entry* {
            return library::find_entry(
                host.model, {std::string(kBuiltIn), library::Kind::mutator, std::string(key)}
            );
        };
        const auto checked_for = [&](std::string_view key) -> std::string {
            const library::Entry* shown_entry = entry_for(key);
            if (shown_entry == nullptr)
                return {};
            for (const library::Fact& fact : library::facts(host.model, *shown_entry))
                if (fact.long_label == library::words::fact_checked)
                    return fact.long_value;
            return {};
        };
        const auto verdict_words = [&] {
            const fs::path path = user_folder_ / std::string(cache_folder_name) /
                                  std::string(oa::data::mutators::verdicts_file_name);
            std::ifstream verdicts(path);
            require(static_cast<bool>(verdicts), "the verdict file is not there");
            std::vector<std::string> words;
            std::string line;
            while (std::getline(verdicts, line)) {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (line.empty())
                    continue;
                std::size_t field = 0;
                std::size_t start = 0;
                std::string word;
                for (std::size_t index = 0; index <= line.size(); ++index) {
                    if (index == line.size() || line[index] == '\t') {
                        if (field == 4)
                            word = line.substr(start, index - start);
                        start = index + 1;
                        ++field;
                    }
                }
                words.push_back(std::move(word));
            }
            return words;
        };
        const auto until_checked = [&](std::string_view key, std::string_view what) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
            while (std::chrono::steady_clock::now() < deadline) {
                const std::string fact = checked_for(key);
                if (fact.find("With No Mod:") != std::string::npos)
                    return;
                frame();
            }
            require(false, std::string(what) + ": " + checked_for(key));
        };
        const auto list_one = [&](std::string_view key, std::string_view name) {
            library::Listing listed;
            listed.registry = std::string(kBuiltIn);
            listed.kind = library::Kind::mutator;
            listed.key = std::string(key);
            listed.name = std::string(name);
            listed.version = "1.0";
            listed.release = 1;
            listed.author = "Ridge Maker";
            listed.size = 1024;
            library::MutatorFacts facts;
            facts.works_with.push_back({"*", {}, {}});
            facts.layers.push_back("units");
            facts.tags.push_back("gameplay");
            listed.mutator = std::move(facts);
            fixture.inputs.listings.push_back(std::move(listed));
            ++fixture.generation;
            list_installed();
            frames();
        };

        list_one(kBuilders, "Ridge Check Builders");
        const fs::path builders_file = packages / "ridge-check-builders.oamut";
        write_checking_mutator(
            builders_file,
            kBuilders,
            "Ridge Check Builders",
            "function mutate(ctx)\n"
            "  return {\n"
            "    rules = {\n"
            "      where { builder = 1 }:change { workertime = scale(2) }\n"
            "    }\n"
            "  }\n"
            "end\n"
        );
        post_catalogue(
            builders_file, kBuiltIn, kBuilders, "Ridge Check Builders", 1, library::Kind::mutator
        );
        installs_quietly("Ridge Check Builders", kBuiltIn, kBuilders);
        show_entry("oa.library.row.check-builtin.ridge-check-builders");
        press_action("check-with-mod");
        require(
            checked_for(kBuilders).find("Checking with No Mod") != std::string::npos,
            "the real check did not say it was running: " + checked_for(kBuilders)
        );
        until_checked(kBuilders, "the real check did not finish");
        const std::string builders_fact = checked_for(kBuilders);
        require(
            builders_fact.find("changes") != std::string::npos &&
                builders_fact.find("unit") != std::string::npos &&
                builders_fact.find("no effect") == std::string::npos &&
                builders_fact.find("changes 0") == std::string::npos,
            "the Checked fact does not read a compatible unit count: " + builders_fact
        );
        {
            const std::vector<std::string> words = verdict_words();
            require(
                words.size() == 1 && words[0] == "compatible",
                "the verdict file does not hold one compatible line"
            );
        }
        const uint32_t runs = mutator_dry_runs_;
        require(runs >= 1, "the dry run did not count");
        ++fixture.generation;
        frames();
        require(
            checked_for(kBuilders) == builders_fact,
            "rebuilding the Library changed the Checked fact"
        );
        require(mutator_dry_runs_ == runs, "rebuilding the Library started another dry run");
        std::cout << "library install check: real verdict " << builders_fact << "\n";

        list_one(kQuiet, "Ridge Check Quiet");
        const fs::path quiet_file = packages / "ridge-check-quiet.oamut";
        write_checking_mutator(
            quiet_file,
            kQuiet,
            "Ridge Check Quiet",
            "function mutate(ctx)\n"
            "  return {\n"
            "    rules = {\n"
            "      where { unitname = is(\"NOUNIT\") }:change { maxdamage = 1 }\n"
            "    }\n"
            "  }\n"
            "end\n"
        );
        post_catalogue(
            quiet_file, kBuiltIn, kQuiet, "Ridge Check Quiet", 1, library::Kind::mutator
        );
        installs_quietly("Ridge Check Quiet", kBuiltIn, kQuiet);
        show_entry("oa.library.row.check-builtin.ridge-check-quiet");
        press_action("check-with-mod");
        until_checked(kQuiet, "the quiet check did not finish");
        const std::string quiet_fact = checked_for(kQuiet);
        require(
            quiet_fact.find("With No Mod: no effect") != std::string::npos,
            "the quiet mutator does not read the no-effect words: " + quiet_fact
        );
        {
            const std::vector<std::string> words = verdict_words();
            require(
                words.size() == 2 && words[0] == "compatible" && words[1] == "no-effect",
                "the verdict file does not hold compatible then no-effect"
            );
        }
        std::cout << "library install check: real verdict " << quiet_fact << "\n";

        constexpr std::string_view kMaps = "ridge-check-maps";
        list_one(kMaps, "Ridge Check Maps");
        const fs::path maps_file = packages / "ridge-check-maps.oamut";
        write_checking_mutator(
            maps_file,
            kMaps,
            "Ridge Check Maps",
            "function mutate(ctx)\n"
            "  return {\n"
            "    maps = function(map, ctx)\n"
            "      map.header.gravity = 10\n"
            "    end\n"
            "  }\n"
            "end\n",
            "maps"
        );
        post_catalogue(maps_file, kBuiltIn, kMaps, "Ridge Check Maps", 1, library::Kind::mutator);
        installs_quietly("Ridge Check Maps", kBuiltIn, kMaps);
        show_entry("oa.library.row.check-builtin.ridge-check-maps");
        press_action("check-with-mod");
        until_checked(kMaps, "the maps check did not finish");
        const std::string maps_fact = checked_for(kMaps);
        require(
            maps_fact.find("changes the map when a match starts") != std::string::npos &&
                maps_fact.find("no effect") == std::string::npos,
            "the maps mutator does not read the map words: " + maps_fact
        );
        {
            const std::vector<std::string> words = verdict_words();
            require(
                words.size() == 3 && words[0] == "compatible" && words[1] == "no-effect" &&
                    words[2] == "compatible",
                "the verdict file does not hold the maps mutator as compatible"
            );
        }
        std::cout << "library install check: real verdict " << maps_fact << "\n";

        // Steady Commander is got and installed, then made the default.
        // Picky Planner and Broken Planner are written into Brains and
        // checked with the runtime's own feed. OPEN FOLDER and HOMEPAGE
        // run while Steady Commander is still installed. UNINSTALL then
        // removes it and clears the default key.
        const fs::path brains_folder =
            user_folder_ / std::string(install::oabrain::brains_folder_name);
        const auto list_brain =
            [&](std::string_view key, std::string_view name, std::string_view homepage) {
                library::Listing listed;
                listed.registry = std::string(kBuiltIn);
                listed.kind = library::Kind::brain;
                listed.key = std::string(key);
                listed.name = std::string(name);
                listed.version = "1.0";
                listed.release = 1;
                listed.author = "Ridge Maker";
                listed.summary = "A made-up brain.";
                listed.homepage = std::string(homepage);
                listed.size = 1024;
                library::BrainFacts facts;
                facts.works_with.push_back({"*", {}, {}});
                facts.tags.push_back("classic");
                listed.brain = std::move(facts);
                fixture.inputs.listings.push_back(std::move(listed));
                ++fixture.generation;
            };
        const auto brain_entry = [&](std::string_view key) -> const library::Entry* {
            return library::find_entry(
                host.model, {std::string(kBuiltIn), library::Kind::brain, std::string(key)}
            );
        };
        const auto brain_checked = [&](std::string_view key) -> std::string {
            const library::Entry* shown_entry = brain_entry(key);
            if (shown_entry == nullptr)
                return {};
            for (const library::Fact& fact : library::facts(host.model, *shown_entry))
                if (fact.long_label == library::words::fact_checked)
                    return fact.long_value;
            return {};
        };
        list_brain(kSteady, "Steady Commander", "https://example.test/steady-commander");
        list_installed();
        open_library(library::Tab::brains);
        frames();
        require(top_is(library_screen_name), "the Library did not open on Brains");
        show_entry("oa.library.row.check-builtin.steady-commander");
        const std::size_t brains_queued = fixture.queued.size();
        press_action("get");
        require(
            fixture.queued.size() == brains_queued + 1 &&
                fixture.queued.back().registry == kBuiltIn &&
                fixture.queued.back().key == kSteady &&
                fixture.queued.back().reason == content::DownloadReason::install,
            "GET did not queue Steady Commander"
        );
        std::cout << "library install check: brain queued " << kSteady << "\n";
        const fs::path steady_file = packages / "steady-commander.oabrain";
        const fs::path steady_brain_folder = brains_folder / std::string(kSteady);
        write_brain(steady_file, kSteady, "Steady Commander", "return {}\n");
        post_catalogue(steady_file, kBuiltIn, kSteady, "Steady Commander", 1, library::Kind::brain);
        installs_quietly("Steady Commander", kBuiltIn, kSteady);
        {
            const std::optional<install::Origin> record = install::read_origin(steady_brain_folder);
            require(
                fs::is_regular_file(steady_brain_folder / "oabrain.yaml") &&
                    fs::is_regular_file(steady_brain_folder / "brain.lua") && record &&
                    record->kind == install::OriginKind::catalogue &&
                    record->registry == kBuiltIn && record->catalogue_id == kSteady &&
                    record->release == 1,
                "Steady Commander is not under Brains with its origin record"
            );
        }
        std::cout << "library install check: brain installed\n";
        show_entry("oa.library.row.check-builtin.steady-commander");
        press_action("use-by-default");
        {
            const auto saved = oa::platform::preferences::load(preference_path_);
            const auto stored = saved.find(std::string(settings::key::brain_default));
            require(
                stored != saved.end() && stored->second == kSteady,
                "USE BY DEFAULT did not store the brain"
            );
            std::cout << "library install check: open-annihilation.brain.default: "
                      << stored->second << "\n";
        }
        fixture.inputs.default_brain = std::string(kSteady);
        ++fixture.generation;
        frames();
        {
            const library::Entry* shown_entry = brain_entry(kSteady);
            require(
                shown_entry != nullptr && shown_entry->is_default &&
                    library::status_line(host.model, *shown_entry) ==
                        library::words::status_installed_default,
                "the default brain does not read Default"
            );
        }
        std::cout << "library install check: brain made default\n";

        const auto install_brain =
            [&](std::string_view key, std::string_view name, std::string_view lua) {
                list_brain(key, name, {});
                const fs::path file = packages / (std::string(key) + ".oabrain");
                write_brain(file, key, name, lua);
                post_catalogue(file, kBuiltIn, key, name, 1, library::Kind::brain);
                installs_quietly(name, kBuiltIn, key);
                require(
                    fs::is_regular_file(brains_folder / std::string(key) / "brain.lua"),
                    std::string(name) + " is not under Brains"
                );
            };
        install_brain(
            kPicky,
            "Picky Planner",
            "return { fits = function() return \"No build list for this mod's third side\" end }\n"
        );
        install_brain(
            kBroken,
            "Broken Planner",
            "return { fits = function() error(\"broken on purpose\") end }\n"
        );
        host.feed.remembered_brain_verdict = [](void* context, std::string_view id) {
            return LibraryHost::own_remembered_brain_verdict(
                static_cast<InstallFeed*>(context)->runtime, id
            );
        };
        host.feed.start_brain_check = [](void* context, std::string_view id) {
            return LibraryHost::own_start_brain_check(
                static_cast<InstallFeed*>(context)->runtime, id
            );
        };
        host.feed.keep_brain_verdict =
            [](void* context, std::string_view id, const library::Verdict& verdict) {
                LibraryHost::own_keep_brain_verdict(
                    static_cast<InstallFeed*>(context)->runtime, id, verdict
                );
            };
        ++fixture.generation;
        frames();
        const auto until_brain = [&](std::string_view key, std::string_view what) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
            while (std::chrono::steady_clock::now() < deadline) {
                const std::string fact = brain_checked(key);
                if (fact.find("With No Mod:") != std::string::npos)
                    return;
                frame();
            }
            require(false, std::string(what) + ": " + brain_checked(key));
        };
        const auto check_brain = [&](std::string_view key, std::string_view expect) {
            show_entry("oa.library.row.check-builtin." + std::string(key));
            press_action("check-with-mod");
            require(
                brain_checked(key).find("Checking with No Mod") != std::string::npos,
                std::string(key) + " did not say it was running: " + brain_checked(key)
            );
            until_brain(key, std::string(key) + " was not checked");
            const std::string fact = brain_checked(key);
            require(fact.find(expect) != std::string::npos, std::string(key) + ": " + fact);
            std::cout << "library install check: brain checked: " << fact << "\n";
        };
        check_brain(kSteady, "it fits this game");
        check_brain(kPicky, "No build list for this mod's third side");
        check_brain(kBroken, "broken on purpose");

        show_entry("oa.library.row.check-builtin.steady-commander");
        require(web_links_ != nullptr, "the run keeps no record of web links");
        const std::size_t brain_opened = user_folder_state().opened.size();
        press_action("open-folder");
        require(
            user_folder_state().opened.size() == brain_opened + 1 &&
                kept_path(user_folder_state().opened.back()) == kept_path(steady_brain_folder),
            "OPEN FOLDER did not show Steady Commander's folder"
        );
        const std::size_t brain_links = web_links_->requests.size();
        press_action("homepage");
        require(
            web_links_->requests.size() == brain_links + 1 &&
                web_links_->requests.back() == "https://example.test/steady-commander",
            "HOMEPAGE did not record Steady Commander's homepage"
        );
        press_action("uninstall");
        {
            const auto* asked = dynamic_cast<const QuestionScreen*>(oa_layer().top());
            require(
                asked != nullptr && oa_layer().find(library_screen_name) != nullptr,
                "UNINSTALL did not ask over the Library"
            );
            const settings::Prompt& shown = asked->question();
            require(shown.title == "Uninstall Steady Commander", "the question's title is wrong");
            require(
                shown.paragraphs.size() == 3 &&
                    shown.paragraphs[0].text ==
                        "Uninstall Steady Commander? Its folder, Brains/steady-commander, is "
                        "removed." &&
                    shown.paragraphs[1].text ==
                        "It is your default computer player: new computer seats go back to the "
                        "built-in player." &&
                    shown.paragraphs[2].text == "GET installs it again.",
                "the question does not name the default brain"
            );
        }
        frame();
        require(
            named("oa.prompt.uninstall").has_value(), "automation does not list oa.prompt.uninstall"
        );
        click_named("oa.prompt.uninstall");
        for (int count = 0;
             count < kMostFrames &&
             (state.discarder.busy() || fs::exists(fs::symlink_status(steady_brain_folder)));
             ++count)
            frame();
        require(
            !fs::exists(fs::symlink_status(steady_brain_folder)),
            "UNINSTALL did not remove Steady Commander's folder"
        );
        {
            const auto saved = oa::platform::preferences::load(preference_path_);
            const auto stored = preference_values_.find(std::string(settings::key::brain_default));
            require(
                saved.find(std::string(settings::key::brain_default)) == saved.end() &&
                    stored == preference_values_.end(),
                "uninstalling the default left the key"
            );
        }
        std::cout << "library install check: brain uninstalled, default key gone\n";
        host.feed.remembered_brain_verdict = nullptr;
        host.feed.start_brain_check = nullptr;
        host.feed.keep_brain_verdict = nullptr;
        {
            std::error_code brain_error;
            fs::remove_all(brains_folder / std::string(kPicky), brain_error);
            fs::remove_all(brains_folder / std::string(kBroken), brain_error);
            require(!brain_error, "a brain folder could not be removed");
        }
        std::erase_if(fixture.inputs.listings, [](const library::Listing& listed) {
            return listed.kind == library::Kind::brain;
        });
        fixture.inputs.default_brain.clear();
        list_installed();

        // About forty more packages, each with an origin record, so one
        // rebuild resolves every shown list from one listing.
        const fs::path bulk_root = user_folder_ / std::string(install::oamut::mutators_folder_name);
        constexpr int kBulk = 40;
        const std::string sha(64, '0');
        for (int index = 1; index <= kBulk; ++index) {
            std::string number = std::to_string(index);
            if (number.size() < 2)
                number.insert(number.begin(), '0');
            const std::string id = "ridge-check-bulk-" + number;
            const fs::path folder = bulk_root / id;
            std::error_code made;
            fs::create_directories(folder, made);
            const std::string manifest = "oamut: 1\n"
                                         "id: " +
                                         id +
                                         "\n"
                                         "name: \"Ridge Check Bulk " +
                                         number +
                                         "\"\n"
                                         "version: \"1.0\"\n"
                                         "author:\n"
                                         "  name: \"Ridge Maker\"\n"
                                         "description: \"A made-up mutator.\"\n"
                                         "tags: [\"gameplay\"]\n"
                                         "works-with: [\"*\"]\n"
                                         "layers: [\"units\"]\n"
                                         "entry: \"mutator.lua\"\n";
            const std::string origin = "oa-origin: 1\n"
                                       "origin: file\n"
                                       "sha256: \"" +
                                       sha +
                                       "\"\n"
                                       "installed: \"2026-01-01\"\n";
            const auto write_one = [](const fs::path& path, std::string_view bytes) {
                std::ofstream out(path, std::ios::binary | std::ios::trunc);
                out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                require(static_cast<bool>(out), "a bulk mutator could not be written");
            };
            write_one(folder / "oamut.yaml", manifest);
            write_one(folder / "mutator.lua", "return {}\n");
            write_one(folder / ".oa-origin.yaml", origin);
        }
        // Eighty more of the player's own, copied by hand with no origin
        // record, so each one's verdict key needs its folder digest: 120 and
        // more in all, as a player who copied a mutator collection has.
        constexpr int kOwn = 80;
        const std::string filler(8192, 'x');
        std::vector<fs::path> own_folders;
        for (int index = 1; index <= kOwn; ++index) {
            std::string number = std::to_string(index);
            if (number.size() < 2)
                number.insert(number.begin(), '0');
            const std::string id = "ridge-check-own-" + number;
            const fs::path folder = bulk_root / id;
            std::error_code made;
            fs::create_directories(folder / "files" / "units", made);
            const std::string manifest = "oamut: 1\n"
                                         "id: " +
                                         id +
                                         "\n"
                                         "name: \"Ridge Check Own " +
                                         number +
                                         "\"\n"
                                         "version: \"1.0\"\n"
                                         "author:\n"
                                         "  name: \"Ridge Maker\"\n"
                                         "description: \"A made-up mutator.\"\n"
                                         "tags: [\"gameplay\"]\n"
                                         "works-with: [\"*\"]\n"
                                         "layers: [\"files\", \"units\"]\n"
                                         "entry: \"mutator.lua\"\n";
            std::ofstream out(folder / "oamut.yaml", std::ios::binary | std::ios::trunc);
            out << manifest;
            std::ofstream lua(folder / "mutator.lua", std::ios::binary | std::ios::trunc);
            lua << "return {}\n";
            std::ofstream unit(folder / "files" / "units" / "RIDGE.FBI", std::ios::binary);
            unit << filler;
            require(
                static_cast<bool>(out) && static_cast<bool>(lua) && static_cast<bool>(unit),
                "a hand-copied mutator could not be written"
            );
            own_folders.push_back(folder);
        }
        list_installed();
        std::size_t mutators_listed = 0;
        for (const library::Installed& installed : fixture.inputs.installed)
            if (installed.kind == library::Kind::mutator)
                ++mutators_listed;
        require(
            mutators_listed >= static_cast<std::size_t>(kBulk + kOwn),
            "the bulk mutators are not listed"
        );
        // The worker's search makes the folder digests off the main thread.
        // This check's feed has no search, so it makes them as one would.
        for (const fs::path& folder : own_folders)
            require(
                host.work.mutator_digests.digest(folder).has_value(),
                "a hand-copied mutator has no folder digest"
            );
        const uint64_t listings_before = host.shown_listings;
        const uint32_t prepares_before = mutator_prepares_;
        const uint64_t reads_before = host.work.mutator_digests.reads();
        const auto rebuild_started = std::chrono::steady_clock::now();
        host.rebuild(*this);
        const auto rebuild_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - rebuild_started
        )
                                    .count();
        require(
            host.shown_listings == listings_before + 1,
            "a rebuild listed the Mutators folder more than once"
        );
        require(mutator_prepares_ == prepares_before, "a rebuild prepared a dry run");
        require(
            host.work.mutator_digests.reads() == reads_before,
            "a rebuild read a mutator folder the search had digested"
        );
        require(rebuild_ms < 250, "a rebuild took " + std::to_string(rebuild_ms) + " ms");
        // A download moves the feed's generation every frame, and the
        // Library rebuilds each time. With nothing installed or searched
        // since, a rebuild keeps the pass: it lists nothing, reads no package
        // and stays well inside a frame.
        constexpr int kFrames = 30;
        const uint64_t listings_kept = host.shown_listings;
        const auto frames_started = std::chrono::steady_clock::now();
        for (int index = 0; index < kFrames; ++index)
            host.rebuild(*this);
        const auto frame_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                  std::chrono::steady_clock::now() - frames_started
                              )
                                  .count() /
                              kFrames;
        require(
            host.shown_listings == listings_kept,
            "a rebuild with nothing changed listed the Mutators folder"
        );
        require(mutator_prepares_ == prepares_before, "a kept rebuild prepared a dry run");
        require(
            host.work.mutator_digests.reads() == reads_before,
            "a rebuild with nothing changed read a mutator folder"
        );
        require(
            frame_us < 4000, "a kept rebuild took " + std::to_string(frame_us) + " microseconds"
        );
        const uint64_t picker_before = host.shown_listings;
        const uint32_t picker_prepares = mutator_prepares_;
        own_picker_inputs(this, oa::ui::mutator_picker::Setup::skirmish);
        require(
            host.shown_listings == picker_before + 1,
            "the picker's list did not list the Mutators folder once"
        );
        require(mutator_prepares_ == picker_prepares, "the picker's list prepared a dry run");
        require(
            host.work.mutator_digests.reads() == reads_before,
            "the picker's list read a mutator folder the search had digested"
        );
        mutator_picker_.reset();
        // START hands the set on from one listing, whatever is installed.
        replace_setup_mutators(
            mutators_memory::Setup::skirmish,
            {oa::ui::mutator_picker::Chosen{"ridge-check-own-01", {}, {}},
             oa::ui::mutator_picker::Chosen{"ridge-check-own-40", {}, {}},
             oa::ui::mutator_picker::Chosen{"ridge-check-bulk-07", {}, {}}}
        );
        const auto start_started = std::chrono::steady_clock::now();
        hand_chosen_mutators(mutators_memory::Setup::skirmish);
        const auto start_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - start_started
        )
                                  .count();
        require(
            chosen_match_mutators().size() == 3, "START did not hand on the three chosen mutators"
        );
        require(start_ms < 250, "START's hand-off took " + std::to_string(start_ms) + " ms");
        replace_setup_mutators(mutators_memory::Setup::skirmish, {});
        forget_mutators(mutators_memory::Setup::skirmish);
        choose_match_mutators({});
        std::cout << "library install check: one listing in " << rebuild_ms
                  << " ms, a kept rebuild in " << frame_us << " us, START's hand-off of three in "
                  << start_ms << " ms with " << mutators_listed << " installed\n";

        require(revision_in(ridge) == 5, "the mutator run changed Ridge");
        // The worker has finished: both checks returned before this restart.
        request_soft_restart();
        return;
    }
    default: {
        // 13. A run nobody watches: SDL's dummy video driver, and none of
        // the prompts the other runs show. A built-in registry's update to
        // Ridge waits in the inbox while the Library is closed, and installs
        // with no prompt once the Library is the top screen, the folder it
        // drops deleted as on a menu someone watches.
        require(
            UserFolderState::unwatched(options_.unattended) && !state.check_shows_prompts,
            "the last run is watched: run the check on SDL's dummy video driver"
        );
        settle();
        require(options_.layered_mod_folder.empty(), "the last run plays a mod");
        require(
            revision_in(ridge) == 5 && recorded_from(ridge, kBuiltIn, 2) &&
                revision_in(kept) == 4 && !install::package_files_waiting(),
            "the last run does not find Ridge's fifth revision with nothing waiting"
        );
        const uint32_t prompts_before = state.prompts_shown;
        post_ridge(kBuiltIn, "1.0", 6, 3);
        for (int count = 0; count < kWaitingFrames; ++count) {
            tell_mod_installs();
            frame();
        }
        {
            const std::optional<install::OpenedPackage> chosen_waiting =
                install::next_package_file();
            require(
                chosen_waiting && chosen_waiting->origin.kind == install::OriginKind::catalogue &&
                    chosen_waiting->origin.registry == kBuiltIn && revision_in(ridge) == 5 &&
                    state.stage == ModInstallState::Stage::idle && state.prompt == nullptr &&
                    state.prompts_shown == prompts_before,
                "a chosen mod did not wait in the inbox with the Library closed and nobody "
                "watching"
            );
        }
        list_installed();
        open_library(library::Tab::mods);
        require(top_is(library_screen_name), "the Library did not open over the main menu");
        installs_quietly("check-builtin's update with nobody watching", kBuiltIn, kRidge);
        require(
            revision_in(ridge) == 6 && recorded_from(ridge, kBuiltIn, 3) &&
                revision_in(kept) == 5 && recorded_from(kept, kBuiltIn, 2) &&
                state.prompts_shown == prompts_before,
            "check-builtin's update with nobody watching did not keep the fifth revision"
        );
        for (int count = 0; count < kMostFrames && state.discarder.busy(); ++count)
            tell_mod_installs();
        require(
            !state.discarder.busy() && !reserved_left(mods),
            "the update with nobody watching left a folder of its own in Mods"
        );
        std::cout << "library install check: nobody watching, a chosen mod waited for the "
                     "Library, then installed with no prompt\n";

        // 14. An added registry's copy over the built-in registry's asks
        // REPLACE MOD over the Library with nobody watching. It waits for its
        // answer, and REPLACE, pressed where automation lists it, installs.
        post_ridge(kAdded, "1.0", 7, 3);
        asks_over_library("check-added's release with nobody watching", "REPLACE MOD");
        for (int count = 0; count < kWaitingFrames; ++count)
            frame();
        require(
            state.prompt != nullptr && oa_layer().top() == state.prompt && titled("REPLACE MOD") &&
                state.prompts_shown == prompts_before + 1 && revision_in(ridge) == 6,
            "REPLACE MOD did not wait for its answer with nobody watching"
        );
        click_named("oa.prompt.replace");
        {
            const std::optional<install::PackageOutcome> outcome =
                until_outcome("REPLACE with nobody watching");
            require(
                outcome && outcome->result == install::OutcomeResult::installed &&
                    state.prompt == nullptr,
                "REPLACE on REPLACE MOD with nobody watching did not install check-added's release"
            );
            take_outcome(*outcome, kAdded, kRidge);
        }
        require(
            top_is(library_screen_name) && revision_in(ridge) == 7 &&
                recorded_from(ridge, kAdded, 3) && revision_in(kept) == 6 &&
                recorded_from(kept, kBuiltIn, 3),
            "REPLACE with nobody watching did not keep check-builtin's copy as the .backup"
        );
        for (int count = 0; count < kMostFrames && state.discarder.busy(); ++count)
            tell_mod_installs();
        require(
            !state.discarder.busy() && !reserved_left(mods),
            "REPLACE with nobody watching left a folder of its own in Mods"
        );
        std::cout << "library install check: nobody watching, REPLACE MOD waited for its "
                     "answer, and REPLACE installed\n";

        // 15. A file opened outside the game waits in the inbox for a run
        // someone watches, with the Library open and closed: nothing is read,
        // asked or installed.
        const fs::path opened_update = packages / "ridge-1.0-r8.oamod";
        write_package(opened_update, profile_text(kRidge, "Ridge", "1.0", 8), 8);
        install::post_package_file(opened_update);
        const uint32_t prompts_with_file = state.prompts_shown;
        const auto file_waits = [&](std::string_view where) {
            for (int count = 0; count < kWaitingFrames; ++count) {
                tell_mod_installs();
                frame();
            }
            const std::optional<install::OpenedPackage> file_waiting = install::next_package_file();
            require(
                file_waiting && file_waiting->origin.kind == install::OriginKind::file &&
                    file_waiting->file.filename() == opened_update.filename() &&
                    state.stage == ModInstallState::Stage::idle && state.prompt == nullptr &&
                    state.prompts_shown == prompts_with_file && revision_in(ridge) == 7,
                "a file opened outside the game did not wait in the inbox " + std::string(where)
            );
        };
        file_waits("with the Library open");
        close_the_library();
        file_waits("on the main menu");
        {
            // Taken back out, so the check leaves nothing waiting.
            const std::optional<install::OpenedPackage> taken = install::take_package_file();
            install::finish_package_file();
            require(
                taken && !install::package_files_waiting(),
                "the file opened outside the game was not the only package waiting"
            );
        }
        std::cout << "library install check: nobody watching, a file opened outside the game "
                     "stayed in the inbox\n";
        std::cout << "library install check: passed\n";
        return;
    }
    }
}

void Runtime::check_library_update_question() {
    // How many times UPDATE is confirmed. Each time the Library must already
    // be showing its tabs two frames later, before the update leaves the list.
    constexpr int kRounds = 50;
    const auto require_step = [](bool ok, std::string_view what) {
        if (!ok)
            throw std::runtime_error("library update question check: " + std::string(what));
    };
    require_step(sdl_.window != nullptr, "the check needs the SDL presenter");
    require_step(!user_folder_.empty(), "the run has no folder of the player's own");
    require_step(
        SDL_SetWindowSize(sdl_.window, 640, 480) && SDL_SyncWindow(sdl_.window),
        std::string("the window did not take 640x480: ") + SDL_GetError()
    );
    apply_output_mode();

    auto& state = mod_install_state();
    state.check_shows_prompts = true;
    const fs::path mods = user_folder_ / std::string(user_mods_folder_name);
    const fs::path packages = user_folder_.parent_path() / "files";
    const fs::path ridge = mods / std::string(kRidge);

    InstallFeed fixture;
    fixture.inputs.registries.push_back(feed_registry(kAdded, "Check Added", false));
    fixture.inputs.engine = content::this_engine_rules().engine;
    fixture.inputs.version_text = "v0.8.0";
    LibraryHost& host = library_host();
    const LibraryFeed own = host.feed;

    struct OwnFeedBack {
        LibraryHost& host;
        LibraryFeed feed;

        ~OwnFeedBack() {
            host.feed = feed;
            host.own_feed = true;
            host.refreshed = false;
        }
    } own_back{host, own};

    host.feed = LibraryFeed{
        &fixture, feed_inputs, feed_generation, feed_enqueue, nullptr, nullptr, feed_built_in
    };
    host.own_feed = false;
    host.refreshed = false;

    bool running = true;
    const auto frame = [&] {
        run_frame(running);
        require_step(running, "a frame ended the run");
    };
    // Two frames: the model follows the feed on the first, after the layer
    // has ticked, and the Library lays itself out on the second. No longer.
    const auto frames = [&] {
        frame();
        frame();
    };
    const auto named = [this](std::string_view name) -> std::optional<AutomationControl> {
        std::vector<AutomationControl> controls;
        oa_layer().automation_controls(controls);
        for (const AutomationControl& control : controls)
            if (control.name == name && control.visible && control.window_width > 0 &&
                control.window_height > 0)
                return control;
        return std::nullopt;
    };
    const auto click_named = [&](std::string_view name) {
        const std::optional<AutomationControl> control = named(name);
        require_step(control.has_value(), std::string(name) + " is not shown");
        const layout::Point pixel{
            control->window_x + control->window_width / 2,
            control->window_y + control->window_height / 2
        };
        for (const SDL_EventType type :
             {SDL_EVENT_MOUSE_MOTION, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP}) {
            SDL_Event event = window_pointer_event(
                sdl_.window,
                sdl_.renderer,
                type,
                pixel,
                type == SDL_EVENT_MOUSE_MOTION ? 0 : SDL_BUTTON_LEFT
            );
            bool going = true;
            dispatch_event(event, going);
            require_step(going, "a click ended the run");
        }
        send_check_pointer(SDL_EVENT_MOUSE_MOTION, kRestingPointer, 0);
    };
    const auto top_is = [this](std::string_view name) {
        const LayerScreen* top = oa_layer().top();
        return top != nullptr && top->name() == name;
    };
    const auto press_action = [&](std::string_view word) {
        const std::string action = "oa.library.action." + std::string(word);
        if (!named(action))
            click_named("oa.library.action");
        click_named(action);
    };
    const auto on_details = [&] {
        return top_is(library_screen_name) && named("oa.library.back").has_value() &&
               !named("oa.library.tab.mods").has_value() &&
               !named("oa.library.tab.updates").has_value();
    };
    const auto tabs_shown = [&] {
        return top_is(library_screen_name) && named("oa.library.tab.mods").has_value() &&
               named("oa.library.tab.updates").has_value() &&
               dynamic_cast<const QuestionScreen*>(oa_layer().top()) == nullptr;
    };

    frames();
    const fs::path package = packages / "ridge-check-added-1.oamod";
    write_package(package, profile_text(kRidge, "Ridge", "1.0", 1), 1);
    install::Origin origin{};
    origin.kind = install::OriginKind::catalogue;
    origin.registry = std::string(kAdded);
    origin.catalogue_id = std::string(kRidge);
    origin.release = 1;
    origin.sha256 = file_hash(package);
    install::post_package_file(package, origin);
    std::optional<install::PackageOutcome> outcome;
    for (int count = 0; count < kMostFrames && !outcome; ++count) {
        tell_mod_installs();
        std::vector<install::PackageOutcome> outcomes = install::take_package_outcomes();
        if (!outcomes.empty())
            outcome = outcomes.front();
        require_step(
            state.prompt == nullptr || state.stage != ModInstallState::Stage::asking,
            "installing Ridge asked a question"
        );
    }
    require_step(
        outcome && outcome->result == install::OutcomeResult::installed &&
            revision_in(ridge) == 1 && recorded_from(ridge, kAdded, 1),
        "Ridge was not installed from the added registry"
    );
    for (int count = 0; count < kMostFrames && state.discarder.busy(); ++count)
        tell_mod_installs();
    require_step(!state.discarder.busy(), "installing Ridge left a folder of its own");

    fixture.inputs.installed.clear();
    std::error_code walk_error;
    for (fs::directory_iterator folder_entry{mods, walk_error}, end;
         !walk_error && folder_entry != end;
         folder_entry.increment(walk_error)) {
        const fs::path folder = folder_entry->path();
        if (path_to_utf8(folder.filename()).starts_with('.'))
            continue;
        const install::InstalledPackage held = install::oamod::read_installed_mod(folder);
        if (held.kind != install::FolderKind::package)
            continue;
        const std::optional<install::Origin> record = install::read_origin(folder);
        library::Installed shown;
        shown.kind = library::Kind::mod;
        shown.key = held.id;
        shown.folder = folder;
        shown.name = held.name;
        shown.version = held.version;
        shown.revision = held.revision;
        if (record && record->kind == install::OriginKind::catalogue) {
            shown.key = record->catalogue_id;
            shown.registry = record->registry;
            shown.release = record->release;
        }
        shown.playing = package_target_in_use(install::mod_kind(), folder);
        fixture.inputs.installed.push_back(std::move(shown));
    }
    oa::base::sha256::Digest old_rules{};
    old_rules[0] = 0x9c;
    oa::base::sha256::Digest new_rules{};
    new_rules[0] = 0x51;
    const auto offer_update = [&] {
        library::UpdateNote update;
        update.registry = std::string(kAdded);
        update.kind = library::Kind::mod;
        update.key = std::string(kRidge);
        update.to_release = 2;
        update.to_version = "1.0";
        update.to_revision = 2;
        update.size = 3355443;
        update.rules = library::UpdateNote::Rules::changes;
        update.from_rules = old_rules;
        update.to_rules = new_rules;
        fixture.inputs.updates.clear();
        fixture.inputs.updates.push_back(std::move(update));
        fixture.inputs.queue.clear();
        ++fixture.generation;
    };
    offer_update();
    open_library(library::Tab::updates);
    frames();
    require_step(top_is(library_screen_name), "the Library did not open");
    const std::string ridge_row = "oa.library.row.check-added.ridge";
    click_named(ridge_row);
    require_step(on_details(), "Ridge's update did not open its details page");

    // NOT NOW leaves the details page, so the player can answer again from it.
    press_action("update");
    frame();
    require_step(
        dynamic_cast<const QuestionScreen*>(oa_layer().top()) != nullptr &&
            oa_layer().find(library_screen_name) != nullptr,
        "the update question was not asked over the Library"
    );
    click_named("oa.prompt.not-now");
    frames();
    require_step(
        on_details() && fixture.queued.empty(), "NOT NOW left the details page or queued the update"
    );

    for (int pass = 1; pass <= kRounds; ++pass) {
        const std::string where = "round " + std::to_string(pass) + ": ";
        require_step(on_details(), where + "the details page was not open");
        press_action("update");
        frame();
        require_step(
            dynamic_cast<const QuestionScreen*>(oa_layer().top()) != nullptr &&
                oa_layer().find(library_screen_name) != nullptr,
            where + "the update question was not asked over the Library"
        );
        click_named("oa.prompt.update");
        frames();
        require_step(tabs_shown(), where + "the Library's tabs were not shown after UPDATE");
        require_step(
            fixture.queued.size() == static_cast<std::size_t>(pass) &&
                fixture.queued.back().registry == kAdded && fixture.queued.back().key == kRidge &&
                fixture.queued.back().reason == content::DownloadReason::update,
            where + "UPDATE did not queue the update"
        );
        click_named("oa.library.tab.mods");
        require_step(
            top_is(library_screen_name) && host.model.tab == library::Tab::mods,
            where + "Mods could not be opened"
        );
        // The update is in place: its note is gone, so the row leaves Updates.
        // The Library stays, with its tabs.
        fixture.inputs.updates.clear();
        ++fixture.generation;
        frames();
        require_step(
            tabs_shown() && host.model.tab == library::Tab::mods,
            where + "the Library was not on top with its tabs once the update was in place"
        );
        if (pass == kRounds)
            break;
        offer_update();
        frames();
        click_named("oa.library.tab.updates");
        require_step(
            top_is(library_screen_name) && host.model.tab == library::Tab::updates,
            where + "Updates could not be opened again"
        );
        click_named(ridge_row);
        require_step(on_details(), where + "the details page did not open again");
    }
    std::cout << "library update question check: the Library stayed open with its tabs for "
              << kRounds << " rounds\n";
}

} // namespace oa::app
