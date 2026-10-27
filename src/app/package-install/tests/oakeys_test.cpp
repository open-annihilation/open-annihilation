// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A key layout installed into a scratch Keys folder: a package with a
// manifest, an icon and a note; a reinstall question; an update that keeps
// .backup and a roll back that swaps the two; a catalogue release of the
// same registry replaced with no question; another registry asking, the
// question naming both; a package inside one top folder; a binding this
// build does not know, listed on the question; and each refusal, with its
// sentence and nothing written. An own folder is left as it was.

#include "oa/app/package_install.hpp"
#include "oa/app/package_install/oakeys.hpp"
#include "oa/base/sha256.hpp"
#include "oa/data/key_library/library.hpp"
#include "oa/formats/oakeys/manifest.hpp"
#include "oa/formats/png.hpp"
#include "oa/formats/zip.hpp"
#include "oa/test/check.hpp"
#include "oa/test/scratch_directory.hpp"
#include "oa/ui/key_bindings/layout_file.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace files = oa::formats::oakeys;
namespace install = oa::app::package_install;
namespace keys = oa::ui::key_bindings;
namespace layouts = install::oakeys;
namespace library = oa::data::key_library;
namespace png = oa::formats::png;
namespace zip = oa::formats::zip;
using install::Change;

/// The layout the tests install.
constexpr std::string_view kId = "grid-test";

/// A scratch folder, deleted when the test ends.
class Scratch {
  public:

    Scratch() : path_(oa::test::make_scratch_directory("oa-package-install-oakeys-")) {}

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

/// A layout that reads.
///
/// @param minor the second version part; the first is 1
/// @param engine its engine requirement; empty when it names none
/// @param id its id
/// @return the manifest
files::Manifest
manifest_of(uint16_t minor, std::string engine = {}, std::string id = std::string(kId)) {
    files::Manifest manifest;
    manifest.format = files::format_version;
    manifest.id = std::move(id);
    manifest.name = "Grid test";
    manifest.version.major = 1;
    manifest.version.minor = minor;
    manifest.version.parts = 2;
    manifest.author.name = "Ada";
    manifest.tags = {"grid"};
    manifest.base = files::Base::oa_default;
    manifest.icon = "icon.png";
    manifest.requires_engine = std::move(engine);
    files::BindingEntry binding;
    binding.action = "order.attack";
    binding.chords.push_back(files::ChordText{"A", 0, 0});
    manifest.bindings.push_back(std::move(binding));
    manifest.helpers.toggle = std::vector<std::string>{};
    manifest.helpers.edge_scroll = files::EdgeScroll::off;
    return manifest;
}

/// The files a good layout holds, and its manifest.
///
/// @param manifest the manifest
/// @param icon the icon
/// @return the package's files
std::vector<Piece> pieces_of(const files::Manifest& manifest, std::vector<uint8_t> icon) {
    std::vector<Piece> pieces;
    pieces.push_back({"oakeys.yaml", bytes_of(files::write_manifest(manifest))});
    if (!manifest.icon.empty())
        pieces.push_back({manifest.icon, std::move(icon)});
    pieces.push_back({"README.md", bytes_of("# Grid test\n")});
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

/// Returns the SHA-256 of a file.
///
/// @param file the file
/// @return its digest
oa::base::sha256::Digest file_hash(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>{in}, {}};
    return oa::base::sha256::digest_of(bytes);
}

/// A catalogue origin for the layout.
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
    install::Incoming incoming{};
};

/// Opens a package, plans it and, when asked, puts the change in place.
///
/// @param file the package
/// @param keys the Keys folder
/// @param origin where it came from
/// @param commit the change to make; nothing plans only
/// @return what it did
Installed open_and_plan(
    const fs::path& file, const fs::path& keys, const install::Origin& origin, const Change* commit
) {
    Installed done{};
    const install::PackageKind* kind = install::find_kind("oakeys");
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
    done.incoming = incoming;
    done.plan = layouts::plan_install(incoming, install::folder_hooks(*kind, keys));
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
    if (!unpacking.start(*opened.package, keys, folder, expected, origin, done.problem))
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
    done.result = install::commit_change(*kind, keys, folder, *commit, options);
    install::Discarder discarder;
    discarder.add(done.result.discards);
    discarder.add(unpacking.discards());
    while (discarder.step()) {
    }
    return done;
}

/// Requires a refusal whose sentence holds a phrase, and that Keys is untouched.
///
/// @param scratch the scratch folder
/// @param pieces the package
/// @param phrase the sentence holds this
/// @param refusal why
/// @param line_phrase a diagnostic line holds this; empty checks none
void expect_refused(
    const Scratch& scratch,
    std::vector<Piece> pieces,
    std::string_view phrase,
    install::Refusal refusal,
    std::string_view line_phrase = {}
) {
    const fs::path file = scratch.path() / "refused.oakeys";
    const fs::path keys = scratch.path() / "Keys";
    write_zip(file, pieces);
    const auto opened = install::open_package(file);
    OA_CHECK(!opened.package);
    OA_CHECK(opened.problem.refusal == refusal);
    const std::string sentence = layouts::refusal_text(opened.problem);
    OA_CHECK(sentence.find(phrase) != std::string::npos);
    if (sentence.find(phrase) == std::string::npos)
        std::fprintf(stderr, "refusal: %s\n", sentence.c_str());
    if (!line_phrase.empty()) {
        bool found = false;
        for (const std::string& line : opened.problem.lines) {
            if (line.find(line_phrase) != std::string::npos)
                found = true;
        }
        OA_CHECK(found);
        if (!found) {
            for (const std::string& line : opened.problem.lines)
                std::fprintf(stderr, "line: %s\n", line.c_str());
        }
    }
    OA_CHECK(!fs::exists(keys));
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

/// Tells whether a question lists a phrase.
///
/// @param prompt the question
/// @param phrase the text
/// @return true when a paragraph holds it
bool lists(const install::PackagePrompt& prompt, std::string_view phrase) {
    for (const auto& paragraph : prompt.prompt.paragraphs) {
        if (paragraph.text.find(phrase) != std::string::npos)
            return true;
    }
    return false;
}

void test_installs(const Scratch& scratch) {
    const fs::path keys = scratch.path() / "install" / "Keys";
    const fs::path packages = scratch.path() / "install";
    const std::vector<uint8_t> icon = png_image(16, 16, png::ColorType::grey);
    const fs::path first_file = packages / "grid-test-1.0.oakeys";
    write_zip(first_file, pieces_of(manifest_of(0), icon));
    const Installed asked = open_and_plan(first_file, keys, file_origin(), nullptr);
    OA_CHECK(asked.plan.kind == install::PlanKind::ask_install);
    OA_CHECK(asked.plan.target == kId && asked.plan.alongside == kId);
    const Change install_change = Change::install;
    const Installed first = open_and_plan(first_file, keys, file_origin(), &install_change);
    OA_CHECK(first.plan.kind == install::PlanKind::ask_install && first.result.changed);
    const fs::path folder = keys / std::string(kId);
    OA_CHECK(fs::exists(folder / "oakeys.yaml"));
    OA_CHECK(fs::exists(folder / "icon.png"));
    OA_CHECK(fs::exists(folder / "README.md"));
    OA_CHECK(!fs::exists(folder / ".backup"));
    {
        std::ifstream in(folder / "oakeys.yaml", std::ios::binary);
        const std::string kept{std::istreambuf_iterator<char>{in}, {}};
        OA_CHECK(kept.find("order.attack") != std::string::npos);
        OA_CHECK(kept.find("helpers:") != std::string::npos);
    }
    {
        const auto record = install::read_origin(folder);
        OA_CHECK(
            record && record->kind == install::OriginKind::file &&
            record->sha256 == file_hash(first_file)
        );
    }

    const Installed again = open_and_plan(first_file, keys, file_origin(), nullptr);
    OA_CHECK(again.plan.kind == install::PlanKind::ask_reinstall);
    const install::PackagePrompt reinstall =
        layouts::question_prompt(again.plan, again.incoming, "grid-test-1.0.oakeys", keys, true);
    OA_CHECK(reinstall.prompt.title == "REINSTALL KEY LAYOUT");
    OA_CHECK(!offers_play(reinstall));

    const fs::path second_file = packages / "grid-test-1.1.oakeys";
    write_zip(second_file, pieces_of(manifest_of(1), icon));
    const Installed update = open_and_plan(second_file, keys, file_origin(), nullptr);
    OA_CHECK(update.plan.kind == install::PlanKind::ask_update);
    const Change replace_change = Change::replace;
    const Installed replaced = open_and_plan(second_file, keys, file_origin(), &replace_change);
    OA_CHECK(replaced.plan.kind == install::PlanKind::ask_update && replaced.result.changed);
    OA_CHECK(replaced.result.backup_kept);
    OA_CHECK(layouts::read_installed(folder).version == "1.1");
    OA_CHECK(layouts::read_installed(folder / ".backup").version == "1.0");

    const install::PackageKind* kind = install::find_kind("oakeys");
    OA_CHECK(kind != nullptr);
    if (kind != nullptr) {
        install::ChangeOptions back{};
        back.first_retry_ms = 0;
        const install::ChangeResult rolled =
            install::commit_change(*kind, keys, std::string(kId), Change::roll_back, back);
        OA_CHECK(rolled.changed);
        OA_CHECK(layouts::read_installed(folder).version == "1.0");
        OA_CHECK(layouts::read_installed(folder / ".backup").version == "1.1");
    }
}

void test_catalogue(const Scratch& scratch) {
    const fs::path keys = scratch.path() / "catalogue" / "Keys";
    const fs::path packages = scratch.path() / "catalogue";
    const std::vector<uint8_t> icon = png_image(16, 16, png::ColorType::grey);
    const fs::path first_file = packages / "grid-test-1.0.oakeys";
    write_zip(first_file, pieces_of(manifest_of(0), icon));
    const Change install_change = Change::install;
    const Installed first =
        open_and_plan(first_file, keys, catalogue_origin("ridge", 1, first_file), &install_change);
    OA_CHECK(first.plan.kind == install::PlanKind::install && !first.plan.replacing);
    OA_CHECK(first.result.changed);
    const fs::path folder = keys / std::string(kId);
    {
        const auto record = install::read_origin(folder);
        OA_CHECK(
            record && record->kind == install::OriginKind::catalogue &&
            record->registry == "ridge" && record->sha256 == file_hash(first_file)
        );
    }

    const fs::path second_file = packages / "grid-test-1.1.oakeys";
    write_zip(second_file, pieces_of(manifest_of(1), icon));
    const Change replace_change = Change::replace;
    const Installed replaced = open_and_plan(
        second_file, keys, catalogue_origin("ridge", 2, second_file), &replace_change
    );
    OA_CHECK(replaced.plan.kind == install::PlanKind::install && replaced.plan.replacing);
    OA_CHECK(replaced.result.changed && replaced.result.backup_kept);
    OA_CHECK(layouts::read_installed(folder).version == "1.1");
    OA_CHECK(fs::exists(folder / ".backup" / "oakeys.yaml"));

    const fs::path third_file = packages / "grid-test-2.oakeys";
    files::Manifest third = manifest_of(0);
    third.version.major = 2;
    third.version.minor = 0;
    write_zip(third_file, pieces_of(third, icon));
    const Installed other =
        open_and_plan(third_file, keys, catalogue_origin("harbour", 1, third_file), nullptr);
    OA_CHECK(other.plan.kind == install::PlanKind::ask_version);
    const install::PackagePrompt prompt =
        layouts::question_prompt(other.plan, other.incoming, "grid-test-2.oakeys", keys, false);
    OA_CHECK(prompt.prompt.title == "REPLACE KEY LAYOUT");
    OA_CHECK(lists(prompt, "from ridge") && lists(prompt, "from harbour"));
    OA_CHECK(layouts::read_installed(folder).version == "1.1");
}

void test_nested(const Scratch& scratch) {
    const fs::path keys = scratch.path() / "nested" / "Keys";
    const fs::path file = scratch.path() / "nested" / "bundled.oakeys";
    std::vector<Piece> pieces = pieces_of(manifest_of(0), png_image(16, 16, png::ColorType::grey));
    for (Piece& piece : pieces)
        piece.name = "bundle/" + piece.name;
    write_zip(file, pieces);
    const Change install_change = Change::install;
    const Installed done =
        open_and_plan(file, keys, catalogue_origin("ridge", 1, file), &install_change);
    OA_CHECK(done.plan.kind == install::PlanKind::install && done.result.changed);
    const fs::path folder = keys / std::string(kId);
    OA_CHECK(fs::exists(folder / "oakeys.yaml"));
    OA_CHECK(fs::exists(folder / "icon.png"));
    OA_CHECK(!fs::exists(keys / "bundle"));
}

void test_unknown(const Scratch& scratch) {
    const fs::path keys = scratch.path() / "unknown" / "Keys";
    const fs::path file = scratch.path() / "unknown" / "grid-test.oakeys";
    files::Manifest manifest = manifest_of(0);
    files::BindingEntry unknown;
    unknown.action = "order.not-yet";
    unknown.chords.push_back(files::ChordText{"Q", 0, 0});
    manifest.bindings.push_back(std::move(unknown));
    write_zip(file, pieces_of(manifest, png_image(16, 16, png::ColorType::grey)));
    const Installed asked = open_and_plan(file, keys, file_origin(), nullptr);
    OA_CHECK(asked.plan.kind == install::PlanKind::ask_install);
    const install::PackagePrompt question =
        layouts::question_prompt(asked.plan, asked.incoming, "grid-test.oakeys", keys, true);
    OA_CHECK(question.prompt.title == "INSTALL KEY LAYOUT");
    OA_CHECK(lists(question, "This version cannot use:"));
    OA_CHECK(lists(question, "order.not-yet: Not in this version of Open Annihilation"));
    const Change install_change = Change::install;
    const Installed done = open_and_plan(file, keys, file_origin(), &install_change);
    OA_CHECK(done.result.changed);
    OA_CHECK(fs::exists(keys / std::string(kId) / "oakeys.yaml"));
    const install::PackagePrompt installed =
        layouts::installed_prompt(done.incoming, keys / std::string(kId), true);
    OA_CHECK(installed.prompt.title == "KEY LAYOUT INSTALLED");
    OA_CHECK(lists(installed, "Choose Use in the Library's Keys tab"));
    OA_CHECK(!offers_play(installed));
}

void test_own_folder(const Scratch& scratch) {
    const fs::path keys = scratch.path() / "own-kept" / "Keys";
    files::Manifest own = manifest_of(0, {}, "own-oa-default");
    own.icon.clear();
    own.bindings.clear();
    own.helpers = {};
    std::string error;
    OA_CHECK(library::write_own(keys, "oa-default", own, error));
    const fs::path folder = keys / "own-oa-default";
    std::ifstream before_in(folder / "oakeys.yaml", std::ios::binary);
    const std::string before{std::istreambuf_iterator<char>{before_in}, {}};
    OA_CHECK(!before.empty());
    install::Incoming incoming{};
    incoming.id = "own-oa-default";
    incoming.name = "Your changes";
    incoming.version = "1.0";
    incoming.origin = file_origin();
    const install::PackageKind* kind = install::find_kind("oakeys");
    OA_CHECK(kind != nullptr);
    if (kind == nullptr)
        return;
    const install::InstallPlan plan =
        layouts::plan_install(incoming, install::folder_hooks(*kind, keys));
    OA_CHECK(plan.kind == install::PlanKind::refuse);
    OA_CHECK(plan.refusal == install::Refusal::not_a_pack);
    install::Problem problem{};
    problem.refusal = plan.refusal;
    problem.subject = incoming.id;
    OA_CHECK(
        layouts::refusal_text(problem) ==
        "The folder own-oa-default does not hold this key layout. It is left as it is."
    );
    std::ifstream after_in(folder / "oakeys.yaml", std::ios::binary);
    const std::string after{std::istreambuf_iterator<char>{after_in}, {}};
    OA_CHECK(after == before);
}

void test_refusals(const Scratch& scratch) {
    const std::vector<uint8_t> icon = png_image(16, 16, png::ColorType::grey);
    const files::Manifest good = manifest_of(0);

    expect_refused(
        scratch,
        {{"README.md", bytes_of("# Grid test\n")}},
        "It holds no oakeys.yaml at its top, or in a single folder at its top.",
        install::Refusal::no_manifest
    );

    {
        files::Manifest bad = good;
        bad.bindings.clear();
        files::BindingEntry binding;
        binding.action = "order.attack";
        binding.chords.push_back(files::ChordText{"Ctrl+Q:sometimes", 0, 0});
        bad.bindings.push_back(std::move(binding));
        const std::string text = files::write_manifest(bad);
        files::Manifest read_back;
        std::vector<files::Problem> ignored;
        OA_CHECK(files::read_manifest(bytes_of(text), read_back, ignored) && ignored.empty());
        keys::Layout layout;
        std::vector<keys::LayoutProblem> layout_problems;
        OA_CHECK(!keys::layout_from_manifest(read_back, nullptr, layout, layout_problems));
        const keys::LayoutProblem* error = nullptr;
        for (const keys::LayoutProblem& one : layout_problems) {
            if (!one.warning)
                error = &one;
        }
        OA_CHECK(error != nullptr);
        if (error != nullptr) {
            const std::string where = "oakeys.yaml:" + std::to_string(error->line) + ":" +
                                      std::to_string(error->column) + ":";
            const fs::path file = scratch.path() / "chord.oakeys";
            write_zip(file, pieces_of(bad, icon));
            const auto opened = install::open_package(file);
            OA_CHECK(!opened.package);
            OA_CHECK(opened.problem.refusal == install::Refusal::manifest_errors);
            OA_CHECK(
                layouts::refusal_text(opened.problem).find("Its oakeys.yaml has errors:") !=
                std::string::npos
            );
            bool found = false;
            for (const std::string& line : opened.problem.lines) {
                if (line.find(where) != std::string::npos &&
                    line.find("unknown gesture") != std::string::npos)
                    found = true;
            }
            OA_CHECK(found);
            if (!found) {
                std::fprintf(stderr, "wanted %s\n", where.c_str());
                for (const std::string& line : opened.problem.lines)
                    std::fprintf(stderr, "line: %s\n", line.c_str());
            }
            OA_CHECK(!fs::exists(scratch.path() / "Keys"));
        }
    }

    expect_refused(
        scratch,
        pieces_of(manifest_of(0, {}, "own-oa-default"), icon),
        "The id own-oa-default is kept for your own changes.",
        install::Refusal::reserved_id
    );

    expect_refused(
        scratch,
        pieces_of(manifest_of(0, {}, "classic"), icon),
        "Its oakeys.yaml has errors:",
        install::Refusal::manifest_errors,
        "built-in layout"
    );

    expect_refused(
        scratch,
        pieces_of(manifest_of(0, {}, "con"), icon),
        "Its id, con, cannot name a folder on Windows.",
        install::Refusal::reserved_id
    );

    auto nested_note = pieces_of(good, icon);
    nested_note.push_back({"notes/bindings.txt", bytes_of("note\n")});
    expect_refused(
        scratch,
        nested_note,
        "The package holds notes/bindings.txt, outside the files a key layout may hold.",
        install::Refusal::outside_layout
    );

    expect_refused(
        scratch,
        pieces_of(good, {0xFF, 0xD8, 0xFF, 0xE0}),
        "Its icon icon.png is not a square PNG of at most 256 pixels and 256 KiB.",
        install::Refusal::icon_invalid
    );

    const std::vector<uint8_t> wide = png_image(300, 300, png::ColorType::grey);
    OA_CHECK(wide.size() <= static_cast<std::size_t>(layouts::most_icon_bytes));
    expect_refused(
        scratch,
        pieces_of(good, wide),
        "Its icon icon.png is not a square PNG of at most 256 pixels and 256 KiB.",
        install::Refusal::icon_invalid
    );

    expect_refused(
        scratch,
        pieces_of(manifest_of(0, ">= 9999.0.0"), icon),
        "needs Open Annihilation 9999.0.0 or later; this is Open Annihilation ",
        install::Refusal::engine_unmet
    );
}

} // namespace

int main() {
    const Scratch scratch;
    test_installs(scratch);
    test_catalogue(scratch);
    test_nested(scratch);
    test_unknown(scratch);
    test_own_folder(scratch);
    test_refusals(scratch);
    return oa::test::check_exit_status();
}
