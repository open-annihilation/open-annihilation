// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The kind table holds oamod, then oamap, then oalang, then expansion, then
// oamut, then oabrain, then oakeys. A made-up kind, built here and not in the table,
// installs, replaces, rolls back and recovers through the same unpacking
// and renames, and its own names are the only ones it leaves in its root
// while it works.

#include "oa/app/package_install.hpp"
#include "oa/formats/zip/stream.hpp"
#include "oa/test/check.hpp"
#include "oa/test/raw_zip.hpp"
#include "oa/test/scratch_directory.hpp"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace install = oa::app::package_install;
namespace raw = oa::test::raw_zip;
namespace zip = oa::formats::zip;

/// A profile that resolves without game data.
///
/// @return the profile's text
std::string oamod_text() {
    return "oamod: 1\n"
           "id: example-mod\n"
           "name: Example Mod\n"
           "version: \"1.0\"\n"
           "description: A made-up mod the tests install.\n"
           "requires: {base: ta-3.1c, catalogue: 1}\n"
           "author: {name: unknown}\n"
           "packaging: {revision: 1, date: 2026-10-04, packager: Open Annihilation}\n";
}

/// Writes bytes to a file.
///
/// @param file the file
/// @param bytes the bytes
void write_bytes(const fs::path& file, const std::vector<uint8_t>& bytes) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(
        reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
    );
}

/// Returns the text of a file.
///
/// @param file the file
/// @return its text
std::string text_of(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// Reads one line of a made-up manifest as the package's id.
///
/// @param manifest the manifest
/// @param context unused
/// @param[in,out] package receives what it installs
/// @param[out] problem why not, when the line is empty
/// @return true when the line names an id
bool read_test_manifest(
    std::span<const uint8_t> manifest,
    const install::ManifestContext&,
    install::Package& package,
    install::Problem& problem
) {
    std::string text(manifest.begin(), manifest.end());
    const std::string id = text.substr(0, text.find('\n'));
    if (id.empty()) {
        problem.refusal = install::Refusal::manifest_errors;
        return false;
    }
    package.incoming = {id, id, "1", 1};
    return true;
}

/// Reads the id from one line of test.yaml.
///
/// @param folder the folder
/// @return what it holds
install::InstalledPackage read_test_installed(const fs::path& folder) {
    install::InstalledPackage installed{};
    std::error_code error;
    if (!fs::exists(folder, error))
        return installed;
    installed.kind = install::FolderKind::other;
    std::ifstream in(folder / "test.yaml", std::ios::binary);
    if (!in)
        return installed;
    std::string id;
    std::getline(in, id);
    if (!id.empty() && id.back() == '\r')
        id.pop_back();
    if (id.empty())
        return installed;
    installed.kind = install::FolderKind::package;
    installed.id = id;
    installed.name = id;
    installed.version = "1";
    installed.revision = 1;
    return installed;
}

/// Reads what a folder's .backup holds, of kind package only for the folder's own id.
///
/// @param folder the folder
/// @return what it holds; nothing when there is no .backup
std::optional<install::InstalledPackage> read_test_backup(const fs::path& folder) {
    const fs::path kept = folder / ".backup";
    std::error_code error;
    if (!fs::exists(kept, error))
        return std::nullopt;
    install::InstalledPackage backup = read_test_installed(kept);
    if (backup.kind == install::FolderKind::missing)
        return std::nullopt;
    const install::InstalledPackage own = read_test_installed(folder);
    if (backup.kind == install::FolderKind::package &&
        (own.kind != install::FolderKind::package || own.id != backup.id))
        backup.kind = install::FolderKind::other;
    return backup;
}

/// The context a staged check must see unchanged.
int staged_marker = 7;

/// Refuses the unpacked files, after checking that the options' context arrived.
///
/// @param staging unused
/// @param package unused
/// @param options the options, whose context is the marker
/// @param[out] problem why the files stay unpacked
/// @return false
bool refuse_staged(
    const fs::path&,
    const install::Package&,
    const install::PackageOptions& options,
    install::Problem& problem
) {
    const auto* marker = static_cast<const int*>(options.context);
    if (marker == nullptr || *marker != 7)
        staged_marker = 0;
    else
        staged_marker = 8;
    problem.refusal = install::Refusal::not_placed;
    problem.detail = "refused before it was put in place";
    return false;
}

/// Fills a package from a zip a made-up kind will unpack.
///
/// @param file the zip
/// @param kind the kind
/// @param manifest the manifest's text, one id line
/// @return the package
install::Package
package_of(const fs::path& file, const install::PackageKind& kind, std::string_view manifest) {
    std::ifstream in(file, std::ios::binary);
    std::vector<uint8_t> bytes{
        std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()
    };
    install::Package package{};
    package.kind = &kind;
    package.file = file;
    package.file_name = file.filename().string();
    package.archive_bytes = bytes.size();
    const zip::SourceHooks source{
        &bytes, [](void* context, uint64_t offset, std::span<uint8_t> out) {
            const auto& data = *static_cast<const std::vector<uint8_t>*>(context);
            if (offset > data.size() || out.size() > data.size() - offset)
                return false;
            std::memcpy(out.data(), data.data() + static_cast<std::size_t>(offset), out.size());
            return true;
        }
    };
    zip::ZipError error{};
    OA_CHECK(
        zip::read_stream_directory(source, package.archive_bytes, {}, package.directory, error)
    );
    for (std::size_t index = 0; index < package.directory.entries.size(); ++index) {
        const zip::StreamEntry& entry = package.directory.entries[index];
        if (entry.directory)
            continue;
        package.files.push_back({index, entry.name});
        package.unpacked_bytes += entry.bytes;
    }
    install::Problem problem{};
    install::PackageOptions options{};
    install::ManifestContext context{};
    context.options = &options;
    context.source = package.file_name + "/test.yaml";
    const std::vector<uint8_t> text(manifest.begin(), manifest.end());
    OA_CHECK(kind.read_manifest(text, context, package, problem));
    return package;
}

/// A file origin with a date, so the unpacking can write its record.
///
/// @return the origin
install::Origin file_origin() {
    install::Origin origin{};
    origin.installed = "2000-01-01";
    return origin;
}

/// Unpacks a package and puts the change in place.
///
/// @param package the package
/// @param root the kind's root
/// @param target the folder
/// @param change the change
/// @param expected what the target must hold
/// @return true when the change landed
bool commit_unpacked(
    const install::Package& package,
    const fs::path& root,
    std::string_view target,
    install::Change change,
    const install::InstalledPackage& expected
) {
    install::Problem problem{};
    install::Unpacking unpacking;
    if (!unpacking.start(package, root, target, expected, file_origin(), problem))
        return false;
    auto step = zip::StreamStep::more;
    while (step == zip::StreamStep::more)
        step = unpacking.step(uint64_t{1} << 20, problem);
    if (step != zip::StreamStep::done)
        return false;
    install::ChangeOptions options{};
    options.expected = expected;
    options.first_retry_ms = 0;
    const install::ChangeResult result =
        install::commit_change(*package.kind, root, target, change, options);
    install::Discarder discarder;
    discarder.add(result.discards);
    while (discarder.step()) {
    }
    return result.changed;
}

void test_table() {
    const auto kinds = install::package_kinds();
    OA_CHECK(kinds.size() == 7);
    OA_CHECK(kinds[0].name == "oamod");
    OA_CHECK(kinds[1].name == "oamap");
    OA_CHECK(kinds[2].name == "oalang");
    OA_CHECK(kinds[3].name == "expansion");
    OA_CHECK(kinds[4].name == "oamut");
    OA_CHECK(kinds[5].name == "oabrain");
    OA_CHECK(kinds[6].name == "oakeys");
    OA_CHECK(install::find_kind("oamod") == &install::mod_kind());
    const install::PackageKind* maps = install::find_kind("oamap");
    OA_CHECK(maps != nullptr && maps == &kinds[1]);
    OA_CHECK(maps != nullptr && maps->root_folder == "Maps" && maps->check_staged != nullptr);
    const install::PackageKind* languages = install::find_kind("oalang");
    OA_CHECK(languages != nullptr && languages == &kinds[2]);
    OA_CHECK(
        languages != nullptr && languages->root_folder == "Languages" &&
        languages->check_staged != nullptr
    );
    OA_CHECK(install::find_kind("oatest") == nullptr);
    OA_CHECK(install::kind_for_file("a.oamod") == &install::mod_kind());
    OA_CHECK(install::kind_for_file("B.OAMOD") == &install::mod_kind());
    OA_CHECK(install::kind_for_file("a.oamap") == maps);
    OA_CHECK(install::kind_for_file("B.OAMAP") == maps);
    OA_CHECK(install::kind_for_file("a.oalang") == languages);
    OA_CHECK(install::kind_for_file("B.OALANG") == languages);
    const install::PackageKind* expansions = install::find_kind("expansion");
    OA_CHECK(expansions != nullptr && expansions == &kinds[3]);
    OA_CHECK(expansions != nullptr && expansions->extension == ".oaexp");
    OA_CHECK(expansions != nullptr && expansions->manifest == "oaexp.yaml");
    OA_CHECK(install::kind_for_file("a.oaexp") == expansions);
    OA_CHECK(install::kind_for_file("B.OAEXP") == expansions);
    const install::PackageKind* mutators = install::find_kind("oamut");
    OA_CHECK(mutators != nullptr && mutators == &kinds[4]);
    OA_CHECK(mutators != nullptr && mutators->extension == ".oamut");
    OA_CHECK(mutators != nullptr && mutators->manifest == "oamut.yaml");
    OA_CHECK(mutators != nullptr && mutators->root_folder == "Mutators");
    OA_CHECK(mutators != nullptr && mutators->check_staged == nullptr);
    OA_CHECK(install::kind_for_file("a.oamut") == mutators);
    OA_CHECK(install::kind_for_file("x.OAMUT") == mutators);
    if (mutators != nullptr) {
        const install::FolderNames mutator_names = install::folder_names(*mutators);
        OA_CHECK(mutator_names.reserved == ".oamut-");
        OA_CHECK(mutator_names.staging == ".oamut-staging-");
        OA_CHECK(mutator_names.old == ".oamut-old-");
        OA_CHECK(mutator_names.replaced == ".oamut-replaced-");
        OA_CHECK(mutator_names.restore == ".oamut-restore-");
        OA_CHECK(mutator_names.discard == ".oamut-discard-");
        OA_CHECK(mutator_names.lock == ".oamut-lock");
    }
    const install::PackageKind* brains = install::find_kind("oabrain");
    OA_CHECK(brains != nullptr && brains == &kinds[5]);
    OA_CHECK(brains != nullptr && brains->extension == ".oabrain");
    OA_CHECK(brains != nullptr && brains->manifest == "oabrain.yaml");
    OA_CHECK(brains != nullptr && brains->root_folder == "Brains");
    OA_CHECK(brains != nullptr && brains->check_staged != nullptr);
    OA_CHECK(install::kind_for_file("a.oabrain") == brains);
    OA_CHECK(install::kind_for_file("x.OABRAIN") == brains);
    if (brains != nullptr) {
        const install::FolderNames brain_names = install::folder_names(*brains);
        OA_CHECK(brain_names.reserved == ".oabrain-");
        OA_CHECK(brain_names.staging == ".oabrain-staging-");
        OA_CHECK(brain_names.old == ".oabrain-old-");
        OA_CHECK(brain_names.replaced == ".oabrain-replaced-");
        OA_CHECK(brain_names.restore == ".oabrain-restore-");
        OA_CHECK(brain_names.discard == ".oabrain-discard-");
        OA_CHECK(brain_names.lock == ".oabrain-lock");
    }
    const install::PackageKind* layouts = install::find_kind("oakeys");
    OA_CHECK(layouts != nullptr && layouts == &kinds[6]);
    OA_CHECK(layouts != nullptr && layouts->extension == ".oakeys");
    OA_CHECK(layouts != nullptr && layouts->manifest == "oakeys.yaml");
    OA_CHECK(layouts != nullptr && layouts->root_folder == "Keys");
    OA_CHECK(layouts != nullptr && layouts->check_staged == nullptr);
    OA_CHECK(install::kind_for_file("a.oakeys") == layouts);
    OA_CHECK(install::kind_for_file("x.OAKEYS") == layouts);
    if (layouts != nullptr) {
        const install::FolderNames layout_names = install::folder_names(*layouts);
        OA_CHECK(layout_names.reserved == ".oakeys-");
        OA_CHECK(layout_names.staging == ".oakeys-staging-");
        OA_CHECK(layout_names.old == ".oakeys-old-");
        OA_CHECK(layout_names.replaced == ".oakeys-replaced-");
        OA_CHECK(layout_names.restore == ".oakeys-restore-");
        OA_CHECK(layout_names.discard == ".oakeys-discard-");
        OA_CHECK(layout_names.lock == ".oakeys-lock");
    }
    OA_CHECK(install::kind_for_file("a.zip") == nullptr);
    OA_CHECK(install::kind_for_file("a.oamod.zip") == nullptr);
    OA_CHECK(install::kind_for_file("oamod") == nullptr);
    const install::FolderNames names = install::folder_names(install::mod_kind());
    OA_CHECK(names.reserved == ".oamod-");
    OA_CHECK(names.staging == ".oamod-staging-");
    OA_CHECK(names.old == ".oamod-old-");
    OA_CHECK(names.replaced == ".oamod-replaced-");
    OA_CHECK(names.restore == ".oamod-restore-");
    OA_CHECK(names.discard == ".oamod-discard-");
    OA_CHECK(names.lock == ".oamod-lock");
    OA_CHECK(languages != nullptr);
    if (languages != nullptr) {
        const install::FolderNames language_names = install::folder_names(*languages);
        OA_CHECK(language_names.reserved == ".oalang-");
        OA_CHECK(language_names.staging == ".oalang-staging-");
        OA_CHECK(language_names.old == ".oalang-old-");
        OA_CHECK(language_names.replaced == ".oalang-replaced-");
        OA_CHECK(language_names.restore == ".oalang-restore-");
        OA_CHECK(language_names.discard == ".oalang-discard-");
        OA_CHECK(language_names.lock == ".oalang-lock");
    }
}

void test_open(const fs::path& scratch) {
    const std::vector<uint8_t> zip =
        raw::build_raw_archive({raw::stored_text("oamod.yaml", oamod_text())}).bytes;
    write_bytes(scratch / "x.zip", zip);
    write_bytes(scratch / "x.OAMOD", zip);
    const auto zip_result = install::open_package(scratch / "x.zip");
    OA_CHECK(!zip_result.package && zip_result.problem.refusal == install::Refusal::unknown_kind);
    const auto oamod_result = install::open_package(scratch / "x.OAMOD");
    OA_CHECK(oamod_result.package.has_value());
    OA_CHECK(oamod_result.package && oamod_result.package->kind == &install::mod_kind());
    OA_CHECK(oamod_result.package && oamod_result.package->incoming.id == "example-mod");
}

/// Tells whether a name is one the made-up kind keeps while it works.
///
/// @param name the file or folder name
/// @return true when it starts with the kind's prefix
bool test_reserved(std::string_view name) {
    return name.starts_with(".oatest-");
}

void test_made_up_kind(const fs::path& scratch) {
    install::PackageKind kind{};
    kind.name = "oatest";
    kind.extension = ".oatest";
    kind.manifest = "test.yaml";
    kind.manifest_most_bytes = 1024;
    kind.root_folder = "Tests";
    kind.prefix = ".oatest-";
    kind.read_manifest = read_test_manifest;
    kind.read_installed = read_test_installed;
    kind.read_backup = read_test_backup;
    kind.check_staged = nullptr;

    const fs::path root = scratch / "Tests";
    const std::vector<uint8_t> first =
        raw::build_raw_archive({
                                   raw::stored_text("test.yaml", "demo\n"),
                                   raw::stored_text("note.txt", "one"),
                               })
            .bytes;
    const std::vector<uint8_t> second =
        raw::build_raw_archive({
                                   raw::stored_text("test.yaml", "demo\n"),
                                   raw::stored_text("note.txt", "two"),
                               })
            .bytes;
    write_bytes(scratch / "first.oatest", first);
    write_bytes(scratch / "second.oatest", second);
    const install::Package installed = package_of(scratch / "first.oatest", kind, "demo\n");
    const install::Package replacement = package_of(scratch / "second.oatest", kind, "demo\n");

    install::Problem problem{};
    install::Unpacking unpacking;
    OA_CHECK(unpacking.start(installed, root, "demo", {}, file_origin(), problem));
    auto step = zip::StreamStep::more;
    while (step == zip::StreamStep::more)
        step = unpacking.step(uint64_t{1} << 20, problem);
    OA_CHECK(step == zip::StreamStep::done);
    bool own_names = true;
    for (const fs::directory_entry& entry : fs::directory_iterator(root)) {
        const std::string name = entry.path().filename().string();
        if (!test_reserved(name))
            own_names = false;
    }
    OA_CHECK(own_names);
    OA_CHECK(fs::exists(root / ".oatest-lock"));
    OA_CHECK(fs::exists(root / ".oatest-staging-demo"));
    install::ChangeOptions options{};
    options.expected = install::InstalledPackage{};
    options.first_retry_ms = 0;
    const install::ChangeResult put =
        install::commit_change(kind, root, "demo", install::Change::install, options);
    OA_CHECK(put.changed);
    OA_CHECK(text_of(root / "demo" / "note.txt") == "one");

    const install::InstalledPackage expected = read_test_installed(root / "demo");
    OA_CHECK(commit_unpacked(replacement, root, "demo", install::Change::replace, expected));
    OA_CHECK(text_of(root / "demo" / "note.txt") == "two");
    OA_CHECK(text_of(root / "demo" / ".backup" / "note.txt") == "one");

    install::ChangeOptions back{};
    back.first_retry_ms = 0;
    const install::ChangeResult rolled =
        install::commit_change(kind, root, "demo", install::Change::roll_back, back);
    OA_CHECK(rolled.changed);
    OA_CHECK(text_of(root / "demo" / "note.txt") == "one");

    fs::rename(root / "demo", root / ".oatest-old-demo");
    const install::Recovery recovery = install::recover_changes(kind, root);
    OA_CHECK(!recovery.skipped);
    OA_CHECK(fs::exists(root / "demo"));
    OA_CHECK(!fs::exists(root / ".oatest-old-demo"));
    OA_CHECK(text_of(root / "demo" / "note.txt") == "one");

    const fs::path discard = root / ".oatest-discard-demo-1";
    fs::create_directory(discard);
    std::ofstream(discard / "gone.txt") << "gone";
    install::Discarder discarder;
    discarder.add(std::vector<fs::path>{discard});
    while (discarder.step()) {
    }
    OA_CHECK(!fs::exists(discard));
    OA_CHECK(fs::exists(root / ".oatest-lock"));

    kind.check_staged = refuse_staged;
    const install::Package blocked = package_of(scratch / "second.oatest", kind, "demo\n");
    install::Unpacking refused;
    const install::InstalledPackage now = read_test_installed(root / "other");
    OA_CHECK(refused.start(blocked, root, "other", now, file_origin(), problem));
    step = zip::StreamStep::more;
    while (step == zip::StreamStep::more)
        step = refused.step(uint64_t{1} << 20, problem);
    OA_CHECK(step == zip::StreamStep::done);
    install::PackageOptions staged_options{};
    staged_options.context = &staged_marker;
    install::Problem staged_problem{};
    OA_CHECK(
        kind.check_staged != nullptr &&
        !kind.check_staged(refused.staging(), blocked, staged_options, staged_problem)
    );
    OA_CHECK(staged_marker == 8);
    OA_CHECK(staged_options.context == &staged_marker);
    refused.cancel();
    OA_CHECK(!fs::exists(root / "other"));
}

} // namespace

int main() {
    const fs::path scratch = oa::test::make_scratch_directory("oa-package-install-kinds-");
    test_table();
    test_open(scratch);
    test_made_up_kind(scratch);
    std::error_code ignored;
    fs::remove_all(scratch, ignored);
    return oa::test::check_exit_status();
}
