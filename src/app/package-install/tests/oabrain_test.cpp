// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A brain installed into a scratch Brains folder: a package with its entry,
// modules, a network, a table, an icon and notes; a reinstall question; an
// update that keeps .backup and a roll back that swaps the two; a catalogue
// release of the same registry replaced with no question; another registry
// asking, the question naming both; a folder that holds something else left
// as it was; an uninstall the discarder deletes; a package inside one top
// folder; and each refusal, with its sentence and nothing written. No script
// is parsed. A staged network whose bytes differ from the manifest is refused
// and the installed brain is left as it was.

#include "oa/app/package_install.hpp"
#include "oa/app/package_install/oabrain.hpp"
#include "oa/base/sha256.hpp"
#include "oa/formats/oabrain/manifest.hpp"
#include "oa/formats/png.hpp"
#include "oa/formats/zip.hpp"
#include "oa/formats/zip/writer.hpp"
#include "oa/test/check.hpp"
#include "oa/test/raw_zip.hpp"
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
namespace install = oa::app::package_install;
namespace brain = oa::formats::oabrain;
namespace oabrain = install::oabrain;
namespace png = oa::formats::png;
namespace raw = oa::test::raw_zip;
namespace sha = oa::base::sha256;
namespace zip = oa::formats::zip;
using install::Change;

/// The brain the tests install.
constexpr std::string_view kId = "steady-commander";

/// A scratch folder, deleted when the test ends.
class Scratch {
  public:

    Scratch() : path_(oa::test::make_scratch_directory("P02-oa-package-install-oabrain-")) {}

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

/// One file of a package.
struct Piece {
    std::string name{};
    std::vector<uint8_t> bytes{};
};

/// Returns text as bytes.
///
/// @param text the text
/// @return its bytes
std::vector<uint8_t> bytes_of(std::string_view text) {
    return {text.begin(), text.end()};
}

/// Writes a stored PNG.
///
/// @param width its width
/// @param height its height
/// @param color its colour type
/// @return the file; empty when it could not be written
std::vector<uint8_t> png_image(uint32_t width, uint32_t height, png::ColorType color) {
    png::Header header{};
    header.width = width;
    header.height = height;
    header.bit_depth = 8;
    header.color_type = color;
    const std::size_t row = png::row_bytes(header, {});
    const std::vector<uint8_t> rows(row * static_cast<std::size_t>(height));
    png::Image image{};
    image.header = header;
    image.rows = rows;
    std::vector<uint8_t> out;
    OA_CHECK(png::write_stored(image, &out));
    return out;
}

/// 300 bytes whose SHA-256 a good manifest names.
///
/// @return the network
std::vector<uint8_t> threat_bytes() {
    std::vector<uint8_t> bytes(300);
    for (std::size_t index = 0; index < bytes.size(); ++index)
        bytes[index] = static_cast<uint8_t>(index);
    return bytes;
}

/// The lower-case hex of a digest.
///
/// @param bytes the message
/// @return 64 hex digits
std::string sha_text(const std::vector<uint8_t>& bytes) {
    const auto hex = sha::to_hex(sha::digest_of(bytes));
    return std::string(hex.begin(), hex.end());
}

/// A brain that reads.
///
/// @param minor the second version part; the first is 1
/// @param engine its engine requirement; empty when it names none
/// @param id its id
/// @param network the bytes of nets/threat.oann; empty names no network
/// @return the manifest
brain::Manifest manifest_of(
    uint16_t minor,
    std::string engine = {},
    std::string id = std::string(kId),
    const std::vector<uint8_t>& network = threat_bytes()
) {
    brain::Manifest manifest;
    manifest.format = brain::format_version;
    manifest.id = std::move(id);
    manifest.name = "Steady Commander";
    manifest.version.major = 1;
    manifest.version.minor = minor;
    manifest.version.parts = 2;
    manifest.author.name = "Ada";
    manifest.description = "Holds the line.";
    manifest.tags = {"classic"};
    manifest.works_with.push_back(oa::formats::oamut::Condition{"*", {}, {}});
    brain::Level level;
    level.id = "steady";
    level.name = "Steady";
    level.difficulty = brain::Difficulty::easy;
    manifest.levels.push_back(std::move(level));
    manifest.icon = "icon.png";
    manifest.requires_engine = std::move(engine);
    if (!network.empty()) {
        brain::Network named;
        named.id = "threat";
        named.file = "nets/threat.oann";
        named.sha256 = sha_text(network);
        manifest.networks.push_back(std::move(named));
    }
    return manifest;
}

/// The files a good brain holds, and its manifest.
///
/// @param manifest the manifest
/// @param icon the icon
/// @param network the network file; empty when the manifest names none
/// @param entry the entry script
/// @return the package's files
std::vector<Piece> pieces_of(
    const brain::Manifest& manifest,
    std::vector<uint8_t> icon,
    std::vector<uint8_t> network = threat_bytes(),
    std::vector<uint8_t> entry = bytes_of("return {}\n")
) {
    std::vector<Piece> pieces;
    pieces.push_back({"oabrain.yaml", bytes_of(brain::write_manifest(manifest))});
    pieces.push_back({manifest.entry, std::move(entry)});
    pieces.push_back({"lua/squads.lua", bytes_of("return {}\n")});
    pieces.push_back({"lua/tasks/land_strike.lua", bytes_of("return {}\n")});
    if (!network.empty() && !manifest.networks.empty())
        pieces.push_back({manifest.networks.front().file, std::move(network)});
    pieces.push_back({"data/build_orders.csv", bytes_of("unit,count\n")});
    if (!manifest.icon.empty())
        pieces.push_back({manifest.icon, std::move(icon)});
    pieces.push_back({"README.md", bytes_of("# Steady Commander\n")});
    pieces.push_back({"LICENSE", bytes_of("Made up.\n")});
    return pieces;
}

/// Writes a stored archive.
///
/// @param file where it goes
/// @param pieces its files
void write_zip(const fs::path& file, const std::vector<Piece>& pieces) {
    std::vector<zip::NewEntry> entries;
    entries.reserve(pieces.size());
    for (const Piece& piece : pieces)
        entries.push_back({piece.name, piece.bytes});
    std::vector<uint8_t> archive;
    zip::ZipError error{};
    OA_CHECK(zip::write_archive(entries, archive, error));
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(
        reinterpret_cast<const char*>(archive.data()), static_cast<std::streamsize>(archive.size())
    );
    OA_CHECK(static_cast<bool>(out));
}

/// Reads a file's bytes.
///
/// @param file the file
/// @return its bytes
std::vector<uint8_t> read_bytes(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>{in}, {}};
}

/// Returns the SHA-256 of a file.
///
/// @param file the file
/// @return its digest
sha::Digest file_hash(const fs::path& file) {
    return sha::digest_of(read_bytes(file));
}

/// A catalogue origin for the brain.
///
/// @param registry the registry
/// @param release the release
/// @param file the package, whose hash is recorded; empty records none yet
/// @return the origin
install::Origin catalogue_origin(std::string registry, int64_t release, const fs::path& file) {
    install::Origin origin{};
    origin.kind = install::OriginKind::catalogue;
    origin.registry = std::move(registry);
    origin.catalogue_id = std::string(kId);
    origin.release = release;
    origin.installed = "2000-01-01";
    if (!file.empty())
        origin.sha256 = file_hash(file);
    return origin;
}

/// A file origin with the date the checks record.
///
/// @return the origin
install::Origin file_origin() {
    install::Origin origin{};
    origin.installed = "2000-01-01";
    return origin;
}

/// What an install did.
struct Installed {
    install::InstallPlan plan{};
    install::ChangeResult result{};
    install::Problem problem{};
    bool staged_ok{};
};

/// Opens a package, plans it and, when asked, puts the change in place.
///
/// When the kind checks staged files, that check runs after unpacking and
/// before the change. A refusal discards the staging.
///
/// @param file the package
/// @param brains the Brains folder
/// @param origin where it came from
/// @param commit the change to make; nothing plans only
/// @return what it did
Installed open_and_plan(
    const fs::path& file,
    const fs::path& brains,
    const install::Origin& origin,
    const Change* commit
) {
    Installed done{};
    const install::PackageKind* kind = install::find_kind("oabrain");
    OA_CHECK(kind != nullptr);
    if (kind == nullptr)
        return done;
    const auto opened = install::open_package(file);
    if (!opened.package) {
        done.problem = opened.problem;
        return done;
    }
    install::Incoming incoming = opened.package->incoming;
    incoming.origin = origin;
    done.plan = oabrain::plan_install(incoming, install::folder_hooks(*kind, brains));
    if (commit == nullptr)
        return done;
    const std::string folder =
        *commit == Change::install && !done.plan.alongside.empty() && done.plan.target.empty()
            ? done.plan.alongside
            : done.plan.target;
    const install::InstalledPackage expected = done.plan.replacing || *commit != Change::install
                                                   ? done.plan.installed
                                                   : install::InstalledPackage{};
    install::Unpacking unpacking;
    if (!unpacking.start(*opened.package, brains, folder, expected, origin, done.problem))
        return done;
    auto step = zip::StreamStep::more;
    while (step == zip::StreamStep::more)
        step = unpacking.step(uint64_t{1} << 20, done.problem);
    if (step != zip::StreamStep::done) {
        unpacking.cancel();
        return done;
    }
    if (kind->check_staged != nullptr) {
        install::PackageOptions options{};
        if (!kind->check_staged(unpacking.staging(), *opened.package, options, done.problem)) {
            unpacking.cancel();
            install::Discarder discarder;
            discarder.add(unpacking.discards());
            while (discarder.step()) {
            }
            return done;
        }
        done.staged_ok = true;
    }
    install::ChangeOptions options{};
    options.expected = expected;
    options.first_retry_ms = 0;
    done.result = install::commit_change(*kind, brains, folder, *commit, options);
    install::Discarder discarder;
    discarder.add(done.result.discards);
    discarder.add(unpacking.discards());
    while (discarder.step()) {
    }
    return done;
}

/// Requires a refusal whose sentence holds a phrase, and that Brains is untouched.
///
/// @param scratch the scratch folder
/// @param pieces the package
/// @param phrase the sentence holds this
/// @param refusal why
void expect_refused(
    const Scratch& scratch,
    std::vector<Piece> pieces,
    std::string_view phrase,
    install::Refusal refusal
) {
    const fs::path file = scratch.path() / "refused.oabrain";
    const fs::path brains = scratch.path() / "Brains";
    write_zip(file, pieces);
    const auto opened = install::open_package(file);
    OA_CHECK(!opened.package);
    OA_CHECK(opened.problem.refusal == refusal);
    const std::string sentence = oabrain::refusal_text(opened.problem);
    OA_CHECK(sentence.find(phrase) != std::string::npos);
    if (sentence.find(phrase) == std::string::npos)
        std::fprintf(stderr, "refusal: %s\n", sentence.c_str());
    OA_CHECK(!fs::exists(brains));
}

/// Tells whether a prompt offers PLAY NOW.
///
/// @param prompt the prompt
/// @return true when a button answers play now
bool offers_play(const install::PackagePrompt& prompt) {
    for (const install::Answer answer : prompt.answers)
        if (answer == install::Answer::play_now)
            return true;
    return false;
}

/// Drops a piece by name.
///
/// @param[in,out] pieces the package
/// @param name the piece
void drop_piece(std::vector<Piece>& pieces, std::string_view name) {
    pieces.erase(
        std::remove_if(
            pieces.begin(), pieces.end(), [&](const Piece& piece) { return piece.name == name; }
        ),
        pieces.end()
    );
}

void test_installs(const Scratch& scratch) {
    const fs::path brains = scratch.path() / "install" / "Brains";
    const fs::path packages = scratch.path() / "install";
    const std::vector<uint8_t> icon = png_image(16, 16, png::ColorType::grey);
    const std::vector<uint8_t> network = threat_bytes();
    const fs::path first_file = packages / "steady-commander-1.0.oabrain";
    write_zip(first_file, pieces_of(manifest_of(0), icon, network));
    const Installed asked = open_and_plan(first_file, brains, file_origin(), nullptr);
    OA_CHECK(asked.plan.kind == install::PlanKind::ask_install);
    OA_CHECK(asked.plan.target == kId && asked.plan.alongside == kId);
    const Change install_change = Change::install;
    const Installed first = open_and_plan(first_file, brains, file_origin(), &install_change);
    OA_CHECK(first.plan.kind == install::PlanKind::ask_install && first.result.changed);
    OA_CHECK(first.staged_ok);
    const fs::path folder = brains / std::string(kId);
    OA_CHECK(fs::exists(folder / "oabrain.yaml"));
    OA_CHECK(fs::exists(folder / "brain.lua"));
    OA_CHECK(fs::exists(folder / "lua" / "squads.lua"));
    OA_CHECK(fs::exists(folder / "lua" / "tasks" / "land_strike.lua"));
    OA_CHECK(fs::exists(folder / "nets" / "threat.oann"));
    OA_CHECK(fs::exists(folder / "data" / "build_orders.csv"));
    OA_CHECK(fs::exists(folder / "icon.png"));
    OA_CHECK(fs::exists(folder / "README.md"));
    OA_CHECK(fs::exists(folder / "LICENSE"));
    OA_CHECK(!fs::exists(folder / ".backup"));
    OA_CHECK(read_bytes(folder / "nets" / "threat.oann") == network);
    {
        const auto record = install::read_origin(folder);
        OA_CHECK(
            record && record->kind == install::OriginKind::file &&
            record->sha256 == file_hash(first_file)
        );
    }

    const Installed again = open_and_plan(first_file, brains, file_origin(), nullptr);
    OA_CHECK(again.plan.kind == install::PlanKind::ask_reinstall);
    install::Incoming same{};
    same.name = "Steady Commander";
    same.version = "1.0";
    same.revision = 0;
    same.origin = file_origin();
    const install::PackagePrompt reinstall =
        oabrain::question_prompt(again.plan, same, "steady-commander-1.0.oabrain", brains, true);
    OA_CHECK(reinstall.prompt.title == "REINSTALL BRAIN");
    OA_CHECK(!reinstall.prompt.paragraphs.empty());
    OA_CHECK(reinstall.prompt.paragraphs[0].text.find("revision 0") != std::string::npos);
    OA_CHECK(!offers_play(reinstall));

    const fs::path second_file = packages / "steady-commander-1.1.oabrain";
    write_zip(second_file, pieces_of(manifest_of(1), icon, network));
    const Installed update = open_and_plan(second_file, brains, file_origin(), nullptr);
    OA_CHECK(update.plan.kind == install::PlanKind::ask_update);
    const Change replace_change = Change::replace;
    const Installed replaced = open_and_plan(second_file, brains, file_origin(), &replace_change);
    OA_CHECK(replaced.plan.kind == install::PlanKind::ask_update && replaced.result.changed);
    OA_CHECK(replaced.result.backup_kept);
    OA_CHECK(replaced.staged_ok);
    OA_CHECK(oabrain::read_installed(folder).version == "1.1");
    OA_CHECK(oabrain::read_installed(folder / ".backup").version == "1.0");

    const install::PackageKind* kind = install::find_kind("oabrain");
    OA_CHECK(kind != nullptr);
    if (kind != nullptr) {
        install::ChangeOptions back{};
        back.first_retry_ms = 0;
        const install::ChangeResult rolled =
            install::commit_change(*kind, brains, std::string(kId), Change::roll_back, back);
        OA_CHECK(rolled.changed);
        OA_CHECK(oabrain::read_installed(folder).version == "1.0");
        OA_CHECK(oabrain::read_installed(folder / ".backup").version == "1.1");
    }
}

void test_catalogue(const Scratch& scratch) {
    const fs::path brains = scratch.path() / "catalogue" / "Brains";
    const fs::path packages = scratch.path() / "catalogue";
    const std::vector<uint8_t> icon = png_image(16, 16, png::ColorType::grey);
    const std::vector<uint8_t> network = threat_bytes();
    const fs::path first_file = packages / "steady-commander-1.0.oabrain";
    write_zip(first_file, pieces_of(manifest_of(0), icon, network));
    const Change install_change = Change::install;
    const Installed first = open_and_plan(
        first_file, brains, catalogue_origin("ridge", 1, first_file), &install_change
    );
    OA_CHECK(first.plan.kind == install::PlanKind::install && !first.plan.replacing);
    OA_CHECK(first.result.changed);
    const fs::path folder = brains / std::string(kId);
    {
        const auto record = install::read_origin(folder);
        OA_CHECK(
            record && record->kind == install::OriginKind::catalogue &&
            record->registry == "ridge" && record->sha256 == file_hash(first_file)
        );
    }

    const fs::path second_file = packages / "steady-commander-1.1.oabrain";
    write_zip(second_file, pieces_of(manifest_of(1), icon, network));
    const Change replace_change = Change::replace;
    const Installed replaced = open_and_plan(
        second_file, brains, catalogue_origin("ridge", 2, second_file), &replace_change
    );
    OA_CHECK(replaced.plan.kind == install::PlanKind::install && replaced.plan.replacing);
    OA_CHECK(replaced.result.changed && replaced.result.backup_kept);
    OA_CHECK(oabrain::read_installed(folder).version == "1.1");
    OA_CHECK(fs::exists(folder / ".backup" / "oabrain.yaml"));

    const fs::path third_file = packages / "steady-commander-2.oabrain";
    brain::Manifest third = manifest_of(0);
    third.version.major = 2;
    third.version.minor = 0;
    write_zip(third_file, pieces_of(third, icon, network));
    const Installed other =
        open_and_plan(third_file, brains, catalogue_origin("harbour", 1, third_file), nullptr);
    OA_CHECK(other.plan.kind == install::PlanKind::ask_version);
    install::Incoming harbour{};
    harbour.id = std::string(kId);
    harbour.name = "Steady Commander";
    harbour.version = "2.0";
    harbour.origin = catalogue_origin("harbour", 1, {});
    const install::PackagePrompt prompt =
        oabrain::question_prompt(other.plan, harbour, "steady-commander-2.oabrain", brains, false);
    OA_CHECK(prompt.prompt.title == "REPLACE BRAIN");
    OA_CHECK(!prompt.prompt.paragraphs.empty());
    OA_CHECK(
        prompt.prompt.paragraphs[0].text.find("from ridge") != std::string::npos &&
        prompt.prompt.paragraphs[0].text.find("from harbour") != std::string::npos
    );
    OA_CHECK(oabrain::read_installed(folder).version == "1.1");
}

void test_not_a_pack(const Scratch& scratch) {
    const fs::path brains = scratch.path() / "own" / "Brains";
    const fs::path file = scratch.path() / "own" / "steady-commander.oabrain";
    write_zip(file, pieces_of(manifest_of(0), png_image(16, 16, png::ColorType::grey)));
    const fs::path notes = brains / std::string(kId) / "notes.txt";
    fs::create_directories(notes.parent_path());
    {
        std::ofstream out(notes, std::ios::binary);
        out << "kept\n";
    }
    OA_CHECK(oabrain::read_installed(brains / std::string(kId)).kind == install::FolderKind::other);
    const Installed refused = open_and_plan(file, brains, file_origin(), nullptr);
    OA_CHECK(refused.plan.kind == install::PlanKind::refuse);
    OA_CHECK(refused.plan.refusal == install::Refusal::not_a_pack);
    install::Problem problem{};
    problem.refusal = refused.plan.refusal;
    problem.subject = std::string(kId);
    OA_CHECK(
        oabrain::refusal_text(problem) ==
        "The folder steady-commander does not hold this brain. It is left as it is."
    );
    std::ifstream in(notes, std::ios::binary);
    const std::string kept{std::istreambuf_iterator<char>{in}, {}};
    OA_CHECK(kept == "kept\n");
}

void test_uninstall(const Scratch& scratch) {
    const fs::path brains = scratch.path() / "remove" / "Brains";
    const fs::path file = scratch.path() / "remove" / "steady-commander.oabrain";
    write_zip(file, pieces_of(manifest_of(0), png_image(16, 16, png::ColorType::grey)));
    const Change install_change = Change::install;
    const Installed done =
        open_and_plan(file, brains, catalogue_origin("ridge", 1, file), &install_change);
    OA_CHECK(done.result.changed);
    const install::PackageKind* kind = install::find_kind("oabrain");
    OA_CHECK(kind != nullptr);
    if (kind == nullptr)
        return;
    install::ChangeOptions options{};
    options.first_retry_ms = 0;
    const install::UninstallResult removed =
        install::uninstall_package(*kind, brains, std::string(kId), options);
    OA_CHECK(removed.removed);
    install::Discarder discarder;
    discarder.add(removed.discards);
    while (discarder.step()) {
    }
    OA_CHECK(!fs::exists(brains / std::string(kId)));
    if (fs::exists(brains)) {
        for (const fs::directory_entry& entry : fs::directory_iterator(brains)) {
            const std::string name = entry.path().filename().string();
            OA_CHECK(
                name != std::string(kId) &&
                (name == ".oabrain-lock" || !name.starts_with(".oabrain-"))
            );
        }
    }
}

void test_nested(const Scratch& scratch) {
    const fs::path brains = scratch.path() / "nested" / "Brains";
    const fs::path file = scratch.path() / "nested" / "bundled.oabrain";
    std::vector<Piece> pieces = pieces_of(manifest_of(0), png_image(16, 16, png::ColorType::grey));
    for (Piece& piece : pieces)
        piece.name = "bundle/" + piece.name;
    write_zip(file, pieces);
    const Change install_change = Change::install;
    const Installed done =
        open_and_plan(file, brains, catalogue_origin("ridge", 1, file), &install_change);
    OA_CHECK(done.plan.kind == install::PlanKind::install && done.result.changed);
    const fs::path folder = brains / std::string(kId);
    OA_CHECK(fs::exists(folder / "oabrain.yaml"));
    OA_CHECK(fs::exists(folder / "brain.lua"));
    OA_CHECK(!fs::exists(brains / "bundle"));
}

void test_known_expansion(const Scratch& scratch) {
    const fs::path file = scratch.path() / "known-expansion.oabrain";
    brain::Manifest manifest = manifest_of(0);
    manifest.requires_expansions.push_back("core-contingency");
    write_zip(file, pieces_of(manifest, png_image(16, 16, png::ColorType::grey)));
    const auto opened = install::open_package(file);
    OA_CHECK(opened.package.has_value());
    OA_CHECK(opened.package && opened.package->incoming.id == kId);
}

/// Where a streamed archive's bytes go: one file, written at a position.
struct FileOut {
    std::fstream file;

    /// Writes bytes at an offset, replacing any already there.
    ///
    /// @param context the FileOut
    /// @param offset where they go
    /// @param bytes the bytes
    /// @return true when they were all written
    static bool write_at(void* context, uint64_t offset, std::span<const uint8_t> bytes) {
        auto* out = static_cast<FileOut*>(context);
        out->file.clear();
        out->file.seekp(static_cast<std::streamoff>(offset));
        out->file.write(
            reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
        );
        return static_cast<bool>(out->file);
    }
};

/// Writes one stored entry of zero bytes.
///
/// @param writer the archive
/// @param name the entry
/// @param bytes how many zeros
/// @param[in,out] error the status
/// @return true when the entry was ended
bool write_zeros(
    zip::StreamWriter& writer, std::string_view name, uint64_t bytes, zip::ZipError& error
) {
    if (!writer.begin_entry(name, bytes, zip::Method::stored, error))
        return false;
    std::vector<uint8_t> piece(std::size_t{64} << 10);
    uint64_t left = bytes;
    while (left > 0) {
        const std::size_t chunk =
            left > piece.size() ? piece.size() : static_cast<std::size_t>(left);
        if (!writer.write(std::span<const uint8_t>(piece.data(), chunk), error))
            return false;
        left -= chunk;
    }
    return writer.end_entry(error);
}

void test_networks_too_large(const Scratch& scratch) {
    brain::Manifest manifest = manifest_of(0, {}, std::string(kId), {});
    manifest.icon.clear();
    brain::Network one;
    one.id = "one";
    one.file = "nets/one.oann";
    one.sha256.assign(64, 'a');
    brain::Network two;
    two.id = "two";
    two.file = "nets/two.oann";
    two.sha256.assign(64, 'b');
    manifest.networks.push_back(std::move(one));
    manifest.networks.push_back(std::move(two));
    const fs::path file = scratch.path() / "networks.oabrain";
    {
        std::ofstream created(file, std::ios::binary | std::ios::trunc);
        OA_CHECK(static_cast<bool>(created));
    }
    FileOut out;
    out.file.open(file, std::ios::in | std::ios::out | std::ios::binary);
    OA_CHECK(static_cast<bool>(out.file));
    zip::StreamWriter writer{{&out, FileOut::write_at}};
    zip::ZipError error{};
    const std::string yaml = brain::write_manifest(manifest);
    const std::vector<Piece> small{
        {"oabrain.yaml", bytes_of(yaml)},
        {"brain.lua", bytes_of("return {}\n")},
        {"lua/squads.lua", bytes_of("return {}\n")},
        {"data/build_orders.csv", bytes_of("unit,count\n")},
        {"README.md", bytes_of("# Steady Commander\n")},
        {"LICENSE", bytes_of("Made up.\n")},
    };
    bool wrote = true;
    for (const Piece& piece : small) {
        wrote =
            wrote && writer.begin_entry(piece.name, piece.bytes.size(), zip::Method::stored, error);
        wrote = wrote && (piece.bytes.empty() || writer.write(piece.bytes, error));
        wrote = wrote && writer.end_entry(error);
    }
    constexpr uint64_t each = uint64_t{33} << 20;
    wrote = wrote && write_zeros(writer, "nets/one.oann", each, error);
    wrote = wrote && write_zeros(writer, "nets/two.oann", each, error);
    wrote = wrote && writer.finish(error);
    OA_CHECK(wrote);
    out.file.close();
    const auto opened = install::open_package(file);
    OA_CHECK(!opened.package);
    OA_CHECK(opened.problem.refusal == install::Refusal::networks_too_large);
    OA_CHECK(opened.problem.size_bytes == each * 2);
    OA_CHECK(opened.problem.limit_bytes == static_cast<uint64_t>(brain::most_network_bytes));
    const std::string sentence = oabrain::refusal_text(opened.problem);
    OA_CHECK(sentence.find("more than the") != std::string::npos);
    if (sentence.find("more than the") == std::string::npos)
        std::fprintf(stderr, "refusal: %s\n", sentence.c_str());
    OA_CHECK(!fs::exists(scratch.path() / "Brains"));
}

void test_network_mismatch(const Scratch& scratch) {
    const fs::path brains = scratch.path() / "hash" / "Brains";
    const fs::path packages = scratch.path() / "hash";
    const std::vector<uint8_t> icon = png_image(16, 16, png::ColorType::grey);
    const std::vector<uint8_t> network = threat_bytes();
    const fs::path good_file = packages / "good.oabrain";
    write_zip(good_file, pieces_of(manifest_of(0), icon, network));
    const Change install_change = Change::install;
    const Installed good = open_and_plan(good_file, brains, file_origin(), &install_change);
    OA_CHECK(good.result.changed && good.staged_ok);
    const fs::path folder = brains / std::string(kId);
    const std::vector<uint8_t> network_before = read_bytes(folder / "nets" / "threat.oann");
    const auto origin_before = install::read_origin(folder);
    OA_CHECK(origin_before.has_value());
    std::vector<uint8_t> flipped = network;
    flipped.back() = static_cast<uint8_t>(flipped.back() + 1);
    const fs::path bad_file = packages / "flipped.oabrain";
    // The manifest still names the original digest. One stored byte differs.
    std::vector<Piece> pieces = pieces_of(manifest_of(0), icon, network);
    for (Piece& piece : pieces)
        if (piece.name == "nets/threat.oann")
            piece.bytes = flipped;
    write_zip(bad_file, pieces);
    const Change reinstall_change = Change::reinstall;
    const Installed refused = open_and_plan(bad_file, brains, file_origin(), &reinstall_change);
    OA_CHECK(!refused.result.changed);
    OA_CHECK(refused.problem.refusal == install::Refusal::network_mismatch);
    OA_CHECK(refused.problem.subject == "nets/threat.oann");
    const std::string sentence = oabrain::refusal_text(refused.problem);
    OA_CHECK(sentence.find("does not match the SHA-256") != std::string::npos);
    OA_CHECK(read_bytes(folder / "nets" / "threat.oann") == network_before);
    const auto origin_after = install::read_origin(folder);
    OA_CHECK(origin_after && origin_before && origin_after->sha256 == origin_before->sha256);
    OA_CHECK(oabrain::read_installed(folder).version == "1.0");
}

void test_refusals(const Scratch& scratch) {
    const std::vector<uint8_t> icon = png_image(16, 16, png::ColorType::grey);
    const std::vector<uint8_t> network = threat_bytes();
    const brain::Manifest good = manifest_of(0);

    expect_refused(
        scratch,
        {{"brain.lua", bytes_of("return {}\n")}},
        "It holds no oabrain.yaml at its top, or in a single folder at its top.",
        install::Refusal::no_manifest
    );

    expect_refused(
        scratch,
        {{"oabrain.yaml",
          bytes_of(
              "oabrain: 1\n"
              "id: \"steady-commander\"\n"
              "name: \"Steady Commander\"\n"
              "version: \"1.0\"\n"
              "author:\n"
              "  name: \"Ada\"\n"
          )},
         {"brain.lua", bytes_of("return {}\n")}},
        "Its oabrain.yaml has errors:",
        install::Refusal::manifest_errors
    );
    {
        const fs::path file = scratch.path() / "broken.oabrain";
        write_zip(
            file,
            {{"oabrain.yaml",
              bytes_of(
                  "oabrain: 1\n"
                  "id: \"steady-commander\"\n"
                  "name: \"Steady Commander\"\n"
                  "version: \"1.0\"\n"
                  "author:\n"
                  "  name: \"Ada\"\n"
              )},
             {"brain.lua", bytes_of("return {}\n")}}
        );
        const auto opened = install::open_package(file);
        OA_CHECK(!opened.package && !opened.problem.lines.empty());
    }

    brain::Manifest moon = good;
    moon.requires_expansions.push_back("moon-base");
    expect_refused(
        scratch,
        pieces_of(moon, icon, network),
        "Its oabrain.yaml has errors:",
        install::Refusal::manifest_errors
    );
    {
        const fs::path file = scratch.path() / "moon.oabrain";
        write_zip(file, pieces_of(moon, icon, network));
        const auto opened = install::open_package(file);
        OA_CHECK(!opened.package);
        OA_CHECK(opened.problem.lines.size() == 1);
        if (opened.problem.lines.size() == 1) {
            OA_CHECK(
                opened.problem.lines[0] ==
                "requires.expansions[0]: moon-base is not an expansion this game knows"
            );
        }
    }

    // Capitals that share no folded name with another file. lua/Tasks would
    // collide with lua/tasks and be refused as a case clash first.
    auto capitals = pieces_of(good, icon, network);
    capitals.push_back({"lua/Capital.lua", bytes_of("return {}\n")});
    expect_refused(
        scratch,
        capitals,
        "The package holds lua/Capital.lua, outside the files a brain may hold.",
        install::Refusal::outside_layout
    );

    auto spare = pieces_of(good, icon, network);
    spare.push_back({"nets/spare.oann", bytes_of("spare\n")});
    expect_refused(
        scratch,
        spare,
        "The package holds nets/spare.oann, outside the files a brain may hold.",
        install::Refusal::outside_layout
    );

    auto notes = pieces_of(good, icon, network);
    notes.push_back({"notes.txt", bytes_of("note\n")});
    expect_refused(
        scratch,
        notes,
        "The package holds notes.txt, outside the files a brain may hold.",
        install::Refusal::outside_layout
    );

    auto missing = pieces_of(good, icon, network);
    drop_piece(missing, "brain.lua");
    expect_refused(
        scratch, missing, "Its script brain.lua is missing.", install::Refusal::script_missing
    );

    std::vector<uint8_t> huge(
        static_cast<std::size_t>(brain::most_script_bytes) + 1, static_cast<uint8_t>('a')
    );
    auto module = pieces_of(good, icon, network);
    for (Piece& piece : module)
        if (piece.name == "lua/squads.lua")
            piece.bytes = huge;
    expect_refused(scratch, module, "is larger than 1 MiB.", install::Refusal::script_missing);

    auto compiled = pieces_of(good, icon, network);
    for (Piece& piece : compiled)
        if (piece.name == "lua/squads.lua")
            piece.bytes = {0x1B, 'L', 'u', 'a'};
    expect_refused(
        scratch, compiled, "lua/squads.lua is precompiled Lua.", install::Refusal::precompiled
    );

    std::vector<uint8_t> jpeg{0xFF, 0xD8, 0xFF, 0xD9};
    expect_refused(
        scratch,
        pieces_of(good, jpeg, network),
        "Its icon icon.png is not a square PNG of at most 256 pixels and 256 KiB.",
        install::Refusal::icon_invalid
    );

    const std::vector<uint8_t> wide = png_image(300, 300, png::ColorType::grey);
    OA_CHECK(wide.size() <= static_cast<std::size_t>(oabrain::most_icon_bytes));
    expect_refused(
        scratch,
        pieces_of(good, wide, network),
        "Its icon icon.png is not a square PNG of at most 256 pixels and 256 KiB.",
        install::Refusal::icon_invalid
    );

    expect_refused(
        scratch,
        pieces_of(good, png_image(200, 100, png::ColorType::grey), network),
        "Its icon icon.png is not a square PNG of at most 256 pixels and 256 KiB.",
        install::Refusal::icon_invalid
    );

    const std::vector<uint8_t> heavy = png_image(512, 512, png::ColorType::grey);
    OA_CHECK(heavy.size() > static_cast<std::size_t>(oabrain::most_icon_bytes));
    expect_refused(
        scratch,
        pieces_of(good, heavy, network),
        "Its icon icon.png is not a square PNG of at most 256 pixels and 256 KiB.",
        install::Refusal::icon_invalid
    );

    auto missing_network = pieces_of(good, icon, network);
    drop_piece(missing_network, "nets/threat.oann");
    expect_refused(
        scratch,
        missing_network,
        "Its network nets/threat.oann is missing.",
        install::Refusal::network_missing
    );

    auto table = pieces_of(good, icon, network);
    for (Piece& piece : table)
        if (piece.name == "data/build_orders.csv")
            piece.bytes.assign(std::size_t{5} << 20, static_cast<uint8_t>('t'));
    expect_refused(
        scratch,
        table,
        "Its table data/build_orders.csv is larger than 4 MiB.",
        install::Refusal::data_too_large
    );

    expect_refused(
        scratch,
        pieces_of(manifest_of(0, ">= 9999.0.0"), icon, network),
        "needs Open Annihilation 9999.0.0 or later; this is Open Annihilation ",
        install::Refusal::engine_unmet
    );

    expect_refused(
        scratch,
        pieces_of(manifest_of(0, {}, "con"), icon, network),
        "Its id, con, cannot name a folder on Windows.",
        install::Refusal::reserved_id
    );
}

void test_installed_prompt() {
    install::Incoming incoming{};
    incoming.name = "Steady Commander";
    incoming.version = "1.0";
    const install::PackagePrompt made =
        oabrain::installed_prompt(incoming, fs::path("Brains") / "steady-commander", true);
    OA_CHECK(made.prompt.title == "BRAIN INSTALLED");
    OA_CHECK(!made.prompt.paragraphs.empty());
    OA_CHECK(
        made.prompt.paragraphs[0].text.find(
            "Choose it for a computer player when you set up a game."
        ) != std::string::npos
    );
    OA_CHECK(!offers_play(made));
    bool open_folder = false;
    bool ok = false;
    for (const install::Answer answer : made.answers) {
        if (answer == install::Answer::open_folder)
            open_folder = true;
        if (answer == install::Answer::ok)
            ok = true;
    }
    OA_CHECK(open_folder && ok);
    install::ChangeResult result{};
    result.changed = true;
    const install::PackagePrompt updated = oabrain::updated_prompt(
        Change::replace,
        incoming,
        install::InstalledPackage{},
        fs::path("Brains") / "steady-commander",
        result,
        true
    );
    OA_CHECK(updated.prompt.title == "BRAIN UPDATED");
    OA_CHECK(!offers_play(updated));
}

} // namespace

int main() {
    const Scratch scratch;
    test_installs(scratch);
    test_catalogue(scratch);
    test_not_a_pack(scratch);
    test_uninstall(scratch);
    test_nested(scratch);
    test_known_expansion(scratch);
    test_networks_too_large(scratch);
    test_network_mismatch(scratch);
    test_refusals(scratch);
    test_installed_prompt();
    return oa::test::check_exit_status();
}
