// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// brain pack and brain check. Pack refuses a folder that is not a brain and
// otherwise runs the same work as pack. Check loads a brain against a game.

#include "command.hpp"

#include "arguments.hpp"
#include "check_brain.hpp"
#include "check_mutator.hpp"

#include "oa/app/brain_check.hpp"
#include "oa/app/brain_seats.hpp"
#include "oa/app/package_install.hpp"
#include "oa/app/package_install/prompts.hpp"
#include "oa/formats/json.hpp"
#include "oa/formats/oabrain/manifest.hpp"
#include "oa/formats/oamut/manifest.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace oa::tool {
namespace {

namespace fs = std::filesystem;

/// Tells whether two texts are equal with ASCII letters folded.
///
/// @param left one text
/// @param right the other
/// @return true when they are equal
bool same_ascii(std::string_view left, std::string_view right) {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto fold = [](unsigned char byte) {
            return byte >= 'A' && byte <= 'Z' ? static_cast<unsigned char>(byte - 'A' + 'a') : byte;
        };
        if (fold(static_cast<unsigned char>(left[index])) !=
            fold(static_cast<unsigned char>(right[index])))
            return false;
    }
    return true;
}

/// Returns a path made from UTF-8 text.
///
/// @param text the text
/// @return the path
fs::path path_of(std::string_view text) {
    return fs::path(std::u8string(text.begin(), text.end()));
}

/// Tells whether a folder's top holds oabrain.yaml.
///
/// @param folder the folder
/// @return true when a file there is named oabrain.yaml, without regard to case
bool top_holds_oabrain(const fs::path& folder) {
    std::error_code error;
    if (!fs::is_directory(fs::symlink_status(folder, error)))
        return false;
    const fs::directory_iterator end;
    for (fs::directory_iterator cursor(folder, error); !error && cursor != end;
         cursor.increment(error)) {
        std::error_code status_error;
        if (fs::is_directory(cursor->symlink_status(status_error)))
            continue;
        const auto name = cursor->path().filename().generic_u8string();
        const std::string text(name.begin(), name.end());
        if (same_ascii(text, "oabrain.yaml"))
            return true;
    }
    return false;
}

/// Prints a usage failure for a brain command.
///
/// @param output where it is printed
/// @param command `brain pack`
/// @param problem what is wrong
void report_brain_usage(Output& output, std::string_view command, std::string_view problem) {
    output.err << "oa-tool " << command << ": " << problem << '\n';
    output.err << "run 'oa-tool help " << command << "'\n";
}

} // namespace

int run_brain_pack(std::span<const std::string> arguments, Output& output) {
    static constexpr OptionSpec options[] = {
        {"out", true, false},
        {"force", false, false},
        {"game-dir", true, false},
    };
    std::string problem;
    const std::optional<Arguments> parsed = parse_arguments(arguments, options, problem);
    if (!parsed || parsed->positional.size() != 1)
        return run_pack(arguments, output);
    if (!top_holds_oabrain(path_of(parsed->positional[0]))) {
        report_brain_usage(output, "brain pack", "the folder holds no oabrain.yaml");
        return exit_usage;
    }
    return run_pack(arguments, output);
}

namespace {

namespace install = oa::app::package_install;
namespace json = oa::formats::json;
namespace oabrain = oa::formats::oabrain;
namespace oamut = oa::formats::oamut;

/// Reads `--side` as a whole number from 0 to 255.
///
/// @param text the option's value
/// @param[out] side the number
/// @param[out] usage the reason, when the text is not such a number
/// @return true when `side` was written
bool parse_side(std::string_view text, uint8_t& side, std::string& usage) {
    if (text.empty()) {
        usage = "option '--side' needs a number from 0 to 255";
        return false;
    }
    unsigned value = 0;
    for (const char byte : text) {
        if (byte < '0' || byte > '9') {
            usage = "option '--side' needs a number from 0 to 255";
            return false;
        }
        value = value * 10u + static_cast<unsigned>(byte - '0');
        if (value > 255u) {
            usage = "option '--side' needs a number from 0 to 255";
            return false;
        }
    }
    side = static_cast<uint8_t>(value);
    return true;
}

/// The last component of a path, as UTF-8.
///
/// @param path the path
/// @return its filename
std::string filename_text(const fs::path& path) {
    const auto name = path.filename().generic_u8string();
    return std::string(name.begin(), name.end());
}

/// One brain read for a check, or why it cannot be checked.
struct LoadedBrain {
    oa::app::InstalledBrain brain;
    std::vector<std::string> problems;
    std::size_t network_bytes{};
    /// Set when the package or folder cannot be read. Empty when `problems`
    /// holds the file check's lines instead.
    std::string failure;
};

struct StoredFiles {
    const std::vector<std::pair<std::string, std::vector<uint8_t>>>* files{};
};

void list_stored(void* context, std::vector<std::string>& paths) {
    const auto* stored = static_cast<const StoredFiles*>(context);
    if (stored == nullptr || stored->files == nullptr)
        return;
    for (const auto& file : *stored->files)
        paths.push_back(file.first);
}

bool read_stored(
    void* context,
    std::string_view path,
    std::size_t limit,
    std::vector<uint8_t>& bytes,
    bool& too_large
) {
    const auto* stored = static_cast<const StoredFiles*>(context);
    too_large = false;
    bytes.clear();
    if (stored == nullptr || stored->files == nullptr)
        return false;
    for (const auto& file : *stored->files) {
        if (file.first != path)
            continue;
        if (file.second.size() > limit) {
            too_large = true;
            return false;
        }
        bytes = file.second;
        return true;
    }
    return false;
}

/// The installer's sentence for a package that would not open, plus its lines.
///
/// @param kind the kind the file's name named
/// @param problem why it would not open
/// @return the text
std::string open_failure(const install::PackageKind& kind, const install::Problem& problem) {
    std::string text = install::refusal_text(kind, problem);
    for (const std::string& line : problem.lines) {
        if (!text.empty())
            text += '\n';
        text += line;
    }
    if (!problem.detail.empty()) {
        if (!text.empty())
            text += '\n';
        text += problem.detail;
    }
    if (text.empty())
        text = "the package could not be opened";
    return text;
}

/// Copies an opened package's files and reads its manifest.
///
/// @param package the package
/// @param[out] loaded the brain, or the problems
void read_packaged(const install::Package& package, LoadedBrain& loaded) {
    const PackageEntries entries = entries_of(package);
    std::vector<std::string> paths;
    if (entries.list != nullptr)
        entries.list(entries.context, paths);
    for (const std::string& path : paths) {
        std::vector<uint8_t> bytes;
        bool too_large = false;
        const bool read = entries.read != nullptr &&
                          entries.read(
                              entries.context,
                              path,
                              static_cast<std::size_t>(oa::app::installed_package_bytes),
                              bytes,
                              too_large
                          );
        if (!read) {
            loaded.problems.push_back(
                path +
                (too_large ? ": it is larger than the package may hold" : ": it cannot be read")
            );
            continue;
        }
        loaded.brain.files.emplace_back(path, std::move(bytes));
    }
    std::sort(
        loaded.brain.files.begin(),
        loaded.brain.files.end(),
        [](const auto& left, const auto& right) { return left.first < right.first; }
    );

    const std::vector<uint8_t>* manifest_bytes = nullptr;
    for (const auto& file : loaded.brain.files) {
        if (same_ascii(file.first, oabrain::manifest_file)) {
            manifest_bytes = &file.second;
            break;
        }
    }
    if (manifest_bytes == nullptr) {
        loaded.problems.emplace_back("oabrain.yaml: it is missing");
        return;
    }
    std::vector<oabrain::Problem> manifest_problems;
    oabrain::Manifest manifest;
    if (!oabrain::read_manifest(*manifest_bytes, manifest, manifest_problems)) {
        if (manifest_problems.empty())
            loaded.problems.emplace_back("oabrain.yaml could not be read");
        for (const oabrain::Problem& item : manifest_problems)
            loaded.problems.push_back(oabrain::describe(item));
        return;
    }
    BrainChecked checked;
    check_brain_files(entries, manifest, loaded.problems, checked);
    loaded.network_bytes = static_cast<std::size_t>(checked.network_bytes);
    loaded.brain.manifest = std::move(manifest);
    loaded.brain.folder = package.file;
}

/// Reads a folder or a package and runs the file checks. The game is not mounted.
///
/// @param file the folder or the package
/// @param directory true when `file` is a folder
/// @return what was read
LoadedBrain load_brain(const fs::path& file, bool directory) {
    LoadedBrain loaded;
    if (directory) {
        std::string failure;
        std::optional<oa::app::InstalledBrain> installed =
            oa::app::read_installed_brain(file.parent_path(), filename_text(file), failure);
        if (!installed) {
            loaded.failure = failure.empty() ? "the brain could not be read" : std::move(failure);
            return loaded;
        }
        loaded.brain = std::move(*installed);
        StoredFiles stored;
        stored.files = &loaded.brain.files;
        const PackageEntries entries{&stored, list_stored, read_stored};
        BrainChecked checked;
        check_brain_files(entries, loaded.brain.manifest, loaded.problems, checked);
        loaded.network_bytes = static_cast<std::size_t>(checked.network_bytes);
        return loaded;
    }

    const install::PackageKind* kind = install::kind_for_file(file);
    const install::PackageResult opened = install::open_package(file);
    if (!opened.package) {
        loaded.failure = kind != nullptr ? open_failure(*kind, opened.problem)
                                         : std::string("the package could not be opened");
        return loaded;
    }
    read_packaged(*opened.package, loaded);
    return loaded;
}

/// Writes one phase as `name: result, N instructions`.
void write_phase(std::ostream& out, std::string_view name, const oa::app::PhaseReport& phase) {
    out << name << ": " << phase.result << ", " << phase.instructions << " instructions\n";
}

/// Writes one phase object. `fits` adds `text` when the brain declined.
void write_phase_json(
    json::JsonWriter& writer, std::string_view name, const oa::app::PhaseReport& phase
) {
    writer.key(name);
    writer.begin_object();
    writer.key("result");
    writer.string(phase.result);
    writer.key("instructions");
    writer.integer(static_cast<int64_t>(phase.instructions));
    if (name == "fits" && phase.result.starts_with("does not fit:")) {
        writer.key("text");
        writer.string(phase.text);
    }
    writer.end_object();
}

void write_string_array(json::JsonWriter& writer, const std::vector<std::string>& lines) {
    writer.begin_array();
    for (const std::string& line : lines)
        writer.string(line);
    writer.end_array();
}

/// Writes the check as one JSON object, keys in the report's order.
void write_json(
    std::ostream& out,
    std::string_view id,
    std::string_view version,
    const std::vector<std::string>& problems,
    const oa::app::BrainCheckReport& report
) {
    json::JsonWriter writer;
    writer.begin_object();
    writer.key("check");
    writer.string("brain");
    writer.key("ok");
    writer.boolean(report.seated);
    writer.key("problems");
    write_string_array(writer, problems);
    writer.key("warnings");
    write_string_array(writer, report.warnings);
    writer.key("id");
    writer.string(id);
    writer.key("version");
    writer.string(version);
    writer.key("level");
    writer.string(report.level);
    writer.key("game");
    writer.string(report.game);
    writer.key("mod");
    if (!report.mod) {
        writer.null();
    } else {
        writer.begin_object();
        writer.key("id");
        writer.string(report.mod->first);
        writer.key("version");
        writer.string(report.mod->second);
        writer.end_object();
    }
    write_phase_json(writer, "load", report.load);
    write_phase_json(writer, "fits", report.fits);
    write_phase_json(writer, "start", report.start);
    writer.key("error");
    if (report.error.empty())
        writer.null();
    else
        writer.string(report.error);
    writer.key("lua_bytes");
    writer.integer(static_cast<int64_t>(report.lua_bytes));
    writer.key("network_bytes");
    writer.integer(static_cast<int64_t>(report.network_bytes));
    writer.end_object();
    out << writer.text() << '\n';
}

/// Writes the text report. A failure's error line is left out when there is none.
void write_text(
    std::ostream& out,
    std::string_view id,
    std::string_view version,
    const oa::app::BrainCheckReport& report
) {
    out << "brain: " << id << ' ' << version << " level " << report.level << '\n';
    out << "game: " << report.game << '\n';
    if (!report.mod)
        out << "mod: none\n";
    else
        out << "mod: " << report.mod->first << ' ' << report.mod->second << '\n';
    write_phase(out, "load", report.load);
    write_phase(out, "fits", report.fits);
    write_phase(out, "start", report.start);
    if (!report.error.empty())
        out << "error: " << report.error << '\n';
    out << "lua: " << report.lua_bytes << " bytes\n";
    out << "networks: " << report.network_bytes << " bytes\n";
    for (const std::string& warning : report.warnings)
        out << "warning: " << warning << '\n';
    out << "result: " << (report.seated ? "ok" : "refused") << '\n';
}

/// A report for a file check that stopped before the game was mounted.
oa::app::BrainCheckReport unrun_report(std::size_t network_bytes) {
    oa::app::BrainCheckReport report;
    report.load.result = "not run";
    report.fits.result = "not run";
    report.start.result = "not run";
    report.network_bytes = network_bytes;
    return report;
}

} // namespace

int run_brain_check(std::span<const std::string> arguments, Output& output) {
    static constexpr OptionSpec options[] = {
        {"game-dir", true, false},
        {"mod", true, false},
        {"level", true, false},
        {"side", true, false},
        {"json", false, false},
    };
    std::string usage;
    const std::optional<Arguments> parsed = parse_arguments(arguments, options, usage);
    if (!parsed || parsed->positional.size() != 1) {
        report_brain_usage(
            output, "brain check", usage.empty() ? "the arguments are not the command's" : usage
        );
        return exit_usage;
    }
    if (parsed->value("game-dir") == nullptr) {
        report_brain_usage(output, "brain check", "option '--game-dir' is required");
        return exit_usage;
    }
    uint8_t side = 0;
    if (const std::string* side_text = parsed->value("side")) {
        if (!parse_side(*side_text, side, usage)) {
            report_brain_usage(output, "brain check", usage);
            return exit_usage;
        }
    }

    const fs::path file = path_of(parsed->positional[0]);
    std::error_code status_error;
    const bool directory = fs::is_directory(fs::symlink_status(file, status_error));
    if (directory) {
        if (!top_holds_oabrain(file)) {
            report_brain_usage(output, "brain check", "the folder holds no oabrain.yaml");
            return exit_usage;
        }
    } else {
        const install::PackageKind* kind = install::kind_for_file(file);
        if (kind == nullptr || kind->name != "oabrain") {
            report_brain_usage(output, "brain check", "the file is not a .oabrain");
            return exit_usage;
        }
    }

    const bool as_json = parsed->flags.contains("json");
    LoadedBrain loaded = load_brain(file, directory);
    if (!loaded.failure.empty()) {
        output.err << loaded.failure << '\n';
        return exit_failed;
    }
    const std::string id = loaded.brain.manifest.id;
    const std::string version = oamut::version_text(loaded.brain.manifest.version);
    if (!loaded.problems.empty()) {
        const oa::app::BrainCheckReport report = unrun_report(loaded.network_bytes);
        if (as_json)
            write_json(output.out, id, version, loaded.problems, report);
        else {
            for (const std::string& line : loaded.problems)
                output.out << "problem: " << line << '\n';
            output.out << "result: refused\n";
        }
        return exit_failed;
    }

    oa::app::BrainCheckRequest request;
    request.game_dir = path_of(*parsed->value("game-dir"));
    if (const std::string* mod = parsed->value("mod"))
        request.mod_folder = path_of(*mod);
    request.choice.id = id;
    if (const std::string* level = parsed->value("level"))
        request.choice.level = *level;
    request.side = side;
    request.brain = std::move(loaded.brain);

    std::string mount_problem;
    const std::optional<oa::app::BrainCheckReport> report =
        oa::app::check_brain_against_game(request, mount_problem);
    if (!report) {
        output.err << mount_problem << '\n';
        return exit_failed;
    }
    if (as_json)
        write_json(output.out, id, version, {}, *report);
    else
        write_text(output.out, id, version, *report);
    return report->seated ? exit_done : exit_failed;
}

} // namespace oa::tool
