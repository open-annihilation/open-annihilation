// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A language pack installed into a scratch Languages folder: the pseudo pack
// plus a font, a Reinstall question for the same package, a Replace that
// keeps the old revision in .backup, a manifest inside one top folder, and
// each refusal, including a font that does not open after unpacking.

#include "oa/app/package_install.hpp"
#include "oa/app/package_install/oalang.hpp"
#include "oa/base/sha256.hpp"
#include "oa/data/languages/language_pack.hpp"
#include "oa/formats/zip.hpp"
#include "oa/platform/text_font.hpp"
#include "oa/test/check.hpp"
#include "oa/test/raw_zip.hpp"
#include "oa/test/scratch_directory.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace install = oa::app::package_install;
namespace languages = oa::data::languages;
namespace oalang = install::oalang;
namespace zip = oa::formats::zip;
using install::Change;

/// The tag the pseudo pack installs.
constexpr std::string_view kTag = "en-XA";

/// A scratch folder, deleted when the test ends.
class Scratch {
  public:

    Scratch() : path_(oa::test::make_scratch_directory("oa-package-install-oalang-")) {}

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

/// Reads a file.
///
/// @param file the file
/// @return its bytes; empty when it cannot be read
std::vector<uint8_t> read_bytes(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>{in}, {}};
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

/// Returns the SHA-256 of a file.
///
/// @param file the file
/// @return its digest
oa::base::sha256::Digest file_hash(const fs::path& file) {
    return oa::base::sha256::digest_of(read_bytes(file));
}

/// A file origin with the date the checks record and the package's hash.
///
/// @param file the package
/// @return the origin
install::Origin file_origin(const fs::path& file) {
    install::Origin origin{};
    origin.installed = "2000-01-01";
    origin.sha256 = file_hash(file);
    return origin;
}

/// Reads every file under a directory.
///
/// @param root the directory
/// @return the files, named below the directory
std::vector<Piece> tree_of(const fs::path& root) {
    std::vector<Piece> pieces;
    std::error_code error;
    for (fs::recursive_directory_iterator at(root, error), end; !error && at != end;
         at.increment(error)) {
        if (!at->is_regular_file(error))
            continue;
        const std::string name = fs::relative(at->path(), root, error).generic_string();
        if (error || name.empty())
            continue;
        pieces.push_back({name, read_bytes(at->path())});
    }
    return pieces;
}

/// Finds a piece by its name.
///
/// @param pieces the pieces
/// @param name the name
/// @return the piece; null when it is missing
Piece* find_piece(std::vector<Piece>& pieces, std::string_view name) {
    for (Piece& piece : pieces)
        if (piece.name == name)
            return &piece;
    return nullptr;
}

/// The pseudo pack, with a font and a packaging revision added.
///
/// @param pseudo the pseudo-pack directory
/// @param revision the packaging revision written into language.yaml
/// @param font the font copied in as fonts/Pseudo.ttf; empty lists it and omits the file
/// @return the package's files
std::vector<Piece>
pseudo_pieces(const fs::path& pseudo, int64_t revision, const std::vector<uint8_t>& font) {
    std::vector<Piece> pieces = tree_of(pseudo);
    Piece* manifest = find_piece(pieces, "language.yaml");
    OA_CHECK(manifest != nullptr);
    if (manifest == nullptr)
        return pieces;
    std::string yaml(manifest->bytes.begin(), manifest->bytes.end());
    yaml += "fonts:\n  - {file: Pseudo.ttf, role: letters}\n";
    yaml += "packaging: {revision: " + std::to_string(revision) +
            ", date: 2026-10-11, packager: Open Annihilation}\n";
    manifest->bytes = bytes_of(yaml);
    if (!font.empty())
        pieces.push_back({"fonts/Pseudo.ttf", font});
    return pieces;
}

/// Replaces one piece's bytes.
///
/// @param pieces the pieces
/// @param name the piece
/// @param bytes its new bytes
void replace_piece(std::vector<Piece>& pieces, std::string_view name, std::vector<uint8_t> bytes) {
    if (Piece* piece = find_piece(pieces, name))
        piece->bytes = std::move(bytes);
}

/// Puts every file inside one top folder.
///
/// @param pieces the pieces
/// @param folder the folder
void nest(std::vector<Piece>& pieces, std::string_view folder) {
    for (Piece& piece : pieces)
        piece.name = std::string(folder) + "/" + piece.name;
}

/// What an install did.
struct Installed {
    install::InstallPlan plan{};
    install::ChangeResult result{};
    install::Problem problem{};
    std::optional<install::Package> package{};
};

/// Opens a package, plans it and, when asked, puts the change in place.
///
/// @param file the package
/// @param languages_root the Languages folder
/// @param origin where it came from
/// @param commit the change to make; nothing plans only
/// @return what it did
Installed open_and_plan(
    const fs::path& file,
    const fs::path& languages_root,
    const install::Origin& origin,
    const Change* commit
) {
    Installed done{};
    const install::PackageKind* kind = install::find_kind("oalang");
    OA_CHECK(kind != nullptr);
    if (kind == nullptr)
        return done;
    const auto opened = install::open_package(file);
    if (!opened.package) {
        done.problem = opened.problem;
        return done;
    }
    done.package = opened.package;
    install::Incoming incoming = opened.package->incoming;
    incoming.origin = origin;
    done.plan = oalang::plan_install(incoming, install::folder_hooks(*kind, languages_root));
    if (commit == nullptr)
        return done;
    const std::string folder = done.plan.target.empty() ? done.plan.alongside : done.plan.target;
    const install::InstalledPackage expected =
        done.plan.replacing || done.plan.reinstalling || *commit != Change::install
            ? done.plan.installed
            : install::InstalledPackage{};
    install::Unpacking unpacking;
    if (!unpacking.start(*opened.package, languages_root, folder, expected, origin, done.problem))
        return done;
    auto step = zip::StreamStep::more;
    while (step == zip::StreamStep::more)
        step = unpacking.step(uint64_t{1} << 20, done.problem);
    if (step != zip::StreamStep::done) {
        unpacking.cancel();
        return done;
    }
    install::ChangeOptions options{};
    options.expected = expected;
    options.first_retry_ms = 0;
    done.result = install::commit_change(*kind, languages_root, folder, *commit, options);
    install::Discarder discarder;
    discarder.add(done.result.discards);
    discarder.add(unpacking.discards());
    while (discarder.step()) {
    }
    return done;
}

/// Requires a refusal, and that Languages was not created.
///
/// @param scratch the scratch folder
/// @param name the package's name
/// @param pieces the package
/// @param phrase the sentence holds this
/// @param refusal why
void expect_refused(
    const Scratch& scratch,
    std::string_view name,
    std::vector<Piece> pieces,
    std::string_view phrase,
    install::Refusal refusal
) {
    const fs::path file = scratch.path() / std::string(name);
    const fs::path languages_root = scratch.path() / (std::string(name) + "-languages");
    write_zip(file, pieces);
    const auto opened = install::open_package(file);
    OA_CHECK(!opened.package);
    OA_CHECK(opened.problem.refusal == refusal);
    const std::string sentence = oalang::refusal_text(opened.problem);
    OA_CHECK(sentence.find(phrase) != std::string::npos);
    if (sentence.find(phrase) == std::string::npos)
        std::fprintf(stderr, "refusal: %s\n", sentence.c_str());
    OA_CHECK(!fs::exists(languages_root));
}

/// Reads an installed language.yaml.
///
/// @param folder the pack's folder
/// @param[out] manifest the manifest
/// @return true when it was read
bool read_installed_manifest(const fs::path& folder, languages::PackManifest& manifest) {
    const std::vector<uint8_t> bytes = read_bytes(folder / "language.yaml");
    return languages::read_manifest(bytes, manifest);
}

void test_installs(
    const Scratch& scratch, const fs::path& pseudo, const std::vector<uint8_t>& font
) {
    const fs::path root = scratch.path() / "install" / "Languages";
    const fs::path packages = scratch.path() / "install";
    const fs::path first_file = packages / "pseudo-1.oalang";
    write_zip(first_file, pseudo_pieces(pseudo, 1, font));
    const Change install_change = Change::install;
    const Installed first =
        open_and_plan(first_file, root, file_origin(first_file), &install_change);
    OA_CHECK(first.plan.kind == install::PlanKind::ask_install);
    OA_CHECK(first.plan.alongside == kTag);
    OA_CHECK(first.result.changed);
    const fs::path folder = root / std::string(kTag);
    OA_CHECK(fs::is_regular_file(folder / "language.yaml"));
    OA_CHECK(fs::is_regular_file(folder / "translate.tdf"));
    OA_CHECK(fs::is_regular_file(folder / "fonts" / "Pseudo.ttf"));
    OA_CHECK(fs::is_regular_file(folder / ".oa-origin.yaml"));
    languages::PackManifest manifest;
    OA_CHECK(read_installed_manifest(folder, manifest));
    OA_CHECK(manifest.tag == kTag);
    OA_CHECK(manifest.packaging && manifest.packaging->revision == 1);
    {
        const auto record = install::read_origin(folder);
        OA_CHECK(record && record->kind == install::OriginKind::file);
    }
    OA_CHECK(first.package.has_value());
    if (first.package) {
        const auto question = oalang::question_prompt(
            first.plan, first.package->incoming, "pseudo-1.oalang", root, false
        );
        OA_CHECK(question.prompt.title.find("INSTALL") != std::string::npos);
        bool alongside = false;
        for (const auto& button : question.prompt.buttons)
            alongside = alongside || button.caption.find("ALONGSIDE") != std::string::npos;
        OA_CHECK(!alongside);
    }

    const Installed again = open_and_plan(first_file, root, file_origin(first_file), nullptr);
    OA_CHECK(again.plan.kind == install::PlanKind::ask_reinstall);
    OA_CHECK(again.package.has_value());
    if (again.package) {
        const auto reinstall = oalang::question_prompt(
            again.plan, again.package->incoming, "pseudo-1.oalang", root, false
        );
        OA_CHECK(reinstall.prompt.title.find("REINSTALL") != std::string::npos);
    }

    const fs::path second_file = packages / "pseudo-2.oalang";
    write_zip(second_file, pseudo_pieces(pseudo, 2, font));
    const Installed asked = open_and_plan(second_file, root, file_origin(second_file), nullptr);
    OA_CHECK(asked.plan.kind == install::PlanKind::ask_update);
    OA_CHECK(asked.package.has_value());
    if (asked.package) {
        const auto replace = oalang::question_prompt(
            asked.plan, asked.package->incoming, "pseudo-2.oalang", root, false
        );
        bool replace_button = false;
        for (const auto& button : replace.prompt.buttons)
            replace_button = replace_button || button.caption.find("Replace") != std::string::npos;
        OA_CHECK(replace_button);
    }
    const Change replace_change = Change::replace;
    const Installed replaced =
        open_and_plan(second_file, root, file_origin(second_file), &replace_change);
    OA_CHECK(replaced.result.changed);
    languages::PackManifest kept;
    OA_CHECK(read_installed_manifest(folder / ".backup", kept));
    OA_CHECK(kept.packaging && kept.packaging->revision == 1);
    languages::PackManifest now;
    OA_CHECK(read_installed_manifest(folder, now));
    OA_CHECK(now.packaging && now.packaging->revision == 2);

    std::vector<Piece> nested = pseudo_pieces(pseudo, 1, font);
    nest(nested, "Language");
    const fs::path nested_root = scratch.path() / "nested" / "Languages";
    const fs::path nested_file = scratch.path() / "nested" / "nested.oalang";
    write_zip(nested_file, nested);
    const Installed inside =
        open_and_plan(nested_file, nested_root, file_origin(nested_file), &install_change);
    OA_CHECK(inside.result.changed);
    OA_CHECK(fs::is_regular_file(nested_root / std::string(kTag) / "language.yaml"));
    OA_CHECK(!fs::exists(nested_root / std::string(kTag) / "Language"));
}

void test_refusals(
    const Scratch& scratch, const fs::path& pseudo, const std::vector<uint8_t>& font
) {
    auto base = pseudo_pieces(pseudo, 1, font);
    auto drop_manifest = base;
    drop_manifest.erase(
        std::remove_if(
            drop_manifest.begin(),
            drop_manifest.end(),
            [](const Piece& piece) { return piece.name == "language.yaml"; }
        ),
        drop_manifest.end()
    );
    expect_refused(
        scratch,
        "no-manifest.oalang",
        drop_manifest,
        "holds no language.yaml",
        install::Refusal::no_manifest
    );

    auto bad = base;
    replace_piece(bad, "language.yaml", bytes_of("oalang: 1\n"));
    expect_refused(scratch, "bad-manifest.oalang", bad, "tag", install::Refusal::manifest_errors);

    auto unmet = base;
    if (Piece* manifest = find_piece(unmet, "language.yaml")) {
        std::string yaml(manifest->bytes.begin(), manifest->bytes.end());
        const std::string from = ">= 0.0.1";
        const auto at = yaml.find(from);
        OA_CHECK(at != std::string::npos);
        if (at != std::string::npos)
            yaml.replace(at, from.size(), ">= 9999.0.0");
        manifest->bytes = bytes_of(yaml);
    }
    expect_refused(scratch, "unmet.oalang", unmet, "9999.0.0", install::Refusal::engine_unmet);

    auto missing_font = pseudo_pieces(pseudo, 1, {});
    expect_refused(
        scratch,
        "missing-font.oalang",
        missing_font,
        "fonts/Pseudo.ttf",
        install::Refusal::font_missing
    );

    auto warmup = base;
    replace_piece(warmup, "warmup.txt", std::vector<uint8_t>(4097, static_cast<uint8_t>('x')));
    expect_refused(scratch, "warmup.oalang", warmup, "4096", install::Refusal::warmup_invalid);

    auto table = base;
    replace_piece(table, "translate.tdf", bytes_of("this is not a table\n"));
    expect_refused(scratch, "table.oalang", table, "translate.tdf", install::Refusal::table_errors);

    const fs::path root = scratch.path() / "font" / "Languages";
    const fs::path good_file = scratch.path() / "font" / "good.oalang";
    write_zip(good_file, base);
    const Change install_change = Change::install;
    const Installed good = open_and_plan(good_file, root, file_origin(good_file), &install_change);
    OA_CHECK(good.result.changed);
    const fs::path folder = root / std::string(kTag);
    const std::vector<uint8_t> before = read_bytes(folder / "language.yaml");
    auto text_font = base;
    replace_piece(text_font, "fonts/Pseudo.ttf", bytes_of("this is not a font\n"));
    const fs::path bad_font = scratch.path() / "font" / "text-font.oalang";
    write_zip(bad_font, text_font);
    const auto opened = install::open_package(bad_font);
    OA_CHECK(opened.package.has_value());
    if (!opened.package)
        return;
    install::Incoming incoming = opened.package->incoming;
    incoming.origin = file_origin(bad_font);
    const install::InstallPlan planned =
        oalang::plan_install(incoming, install::folder_hooks(*opened.package->kind, root));
    OA_CHECK(planned.kind == install::PlanKind::ask_reinstall);
    install::Problem problem{};
    install::Unpacking unpacking;
    OA_CHECK(unpacking.start(
        *opened.package, root, std::string(kTag), planned.installed, incoming.origin, problem
    ));
    auto step = zip::StreamStep::more;
    while (step == zip::StreamStep::more)
        step = unpacking.step(uint64_t{1} << 20, problem);
    OA_CHECK(step == zip::StreamStep::done);
    install::PackageOptions options{};
    install::Problem unreadable{};
    OA_CHECK(opened.package->kind != nullptr && opened.package->kind->check_staged != nullptr);
    if (opened.package->kind != nullptr && opened.package->kind->check_staged != nullptr)
        OA_CHECK(!opened.package->kind->check_staged(
            unpacking.staging(), *opened.package, options, unreadable
        ));
    OA_CHECK(unreadable.refusal == install::Refusal::font_unreadable);
    OA_CHECK(oalang::refusal_text(unreadable).find("fonts/Pseudo.ttf") != std::string::npos);
    unpacking.cancel();
    install::Discarder discarder;
    discarder.add(unpacking.discards());
    while (discarder.step()) {
    }
    OA_CHECK(read_bytes(folder / "language.yaml") == before);

    const auto unsafe = oa::test::raw_zip::build_raw_archive(
        {oa::test::raw_zip::stored_text("../x", "nope"),
         oa::test::raw_zip::stored_text("language.yaml", "oalang: 1\ntag: en-XA\nword: Pseudo\n")}
    );
    const fs::path unsafe_file = scratch.path() / "unsafe.oalang";
    {
        std::ofstream out(unsafe_file, std::ios::binary | std::ios::trunc);
        out.write(
            reinterpret_cast<const char*>(unsafe.bytes.data()),
            static_cast<std::streamsize>(unsafe.bytes.size())
        );
        OA_CHECK(static_cast<bool>(out));
    }
    const auto refused = install::open_package(unsafe_file);
    OA_CHECK(!refused.package);
    OA_CHECK(refused.problem.refusal == install::Refusal::unsafe_name);
    OA_CHECK(!fs::exists(scratch.path() / "unsafe-languages"));
}

} // namespace

int main(int argc, char** argv) {
    OA_CHECK(argc >= 2);
    if (argc < 2)
        return oa::test::check_exit_status();
    const fs::path pseudo = argv[1];
    const fs::path font_file = oa::platform::text_font::bundled_font_directory() / "DejaVuSans.ttf";
    const std::vector<uint8_t> font = read_bytes(font_file);
    OA_CHECK(!font.empty());
    if (font.empty())
        return oa::test::check_exit_status();
    const Scratch scratch;
    test_installs(scratch, pseudo, font);
    test_refusals(scratch, pseudo, font);
    return oa::test::check_exit_status();
}
