// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Checks a brain's layout, compiles its Lua and hashes its networks. Nothing
// is run, and a network file is not interpreted. The facts are the ones a
// catalogue entry copies.

#include "check_brain.hpp"

#include "oa/app/package_install/oabrain.hpp"
#include "oa/base/sha256.hpp"
#include "oa/data/script_host/script_host.hpp"
#include "oa/formats/oamod.hpp"
#include "oa/formats/png.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::tool {
namespace {

namespace brain = oa::formats::oabrain;
namespace install = oa::app::package_install;
namespace json = oa::formats::json;
namespace mut = oa::formats::oamut;
namespace oamod = oa::formats::oamod;
namespace png = oa::formats::png;
namespace script = oa::data::script_host;
namespace sha256 = oa::base::sha256;

static_assert(brain::most_script_bytes == (std::size_t{1} << 20));
static_assert(brain::most_network_bytes == (std::size_t{64} << 20));
static_assert(brain::most_data_bytes == (std::size_t{4} << 20));
static_assert(install::oabrain::most_icon_side == 256);
static_assert(install::oabrain::most_icon_bytes == (uint64_t{256} << 10));

/// The words for an icon the limits refuse.
constexpr std::string_view icon_refusal =
    "it is not a square PNG of at most 256 pixels and 256 KiB";

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

/// Tells whether left sorts before right in unsigned byte order.
///
/// @param left one path
/// @param right the other
/// @return true when left comes first
bool bytes_before(std::string_view left, std::string_view right) {
    const auto* left_bytes = reinterpret_cast<const uint8_t*>(left.data());
    const auto* right_bytes = reinterpret_cast<const uint8_t*>(right.data());
    return std::lexicographical_compare(
        left_bytes, left_bytes + left.size(), right_bytes, right_bytes + right.size()
    );
}

/// One problem, as `<path>:<line>: <message>`.
///
/// @param path the package path
/// @param line the line, from 1, or 0 when the line is unknown
/// @param message what is wrong
/// @return the line
std::string problem_at(std::string_view path, int64_t line, std::string_view message) {
    std::string text{path};
    text.push_back(':');
    text += std::to_string(line);
    text += ": ";
    text.append(message);
    return text;
}

/// Reads one entry, or reports that it is missing or too large.
///
/// @param entries the reader
/// @param path the path
/// @param limit the most bytes
/// @param[out] bytes the file
/// @param[out] too_large true when the file is larger than limit
/// @return true when the file was read
bool read_limited(
    const PackageEntries& entries,
    std::string_view path,
    std::size_t limit,
    std::vector<uint8_t>& bytes,
    bool& too_large
) {
    bytes.clear();
    too_large = false;
    if (entries.read == nullptr)
        return false;
    return entries.read(entries.context, path, limit, bytes, too_large);
}

/// Compiles one Lua source and appends a problem when it does not.
///
/// @param entries the reader
/// @param path the source
/// @param[in,out] problems the problems
/// @param[in,out] scripts how many sources have compiled, counting this one when it does
void compile_one(
    const PackageEntries& entries,
    std::string_view path,
    std::vector<std::string>& problems,
    uint32_t& scripts
) {
    std::vector<uint8_t> bytes;
    bool too_large = false;
    if (!read_limited(entries, path, brain::most_script_bytes, bytes, too_large)) {
        if (too_large)
            problems.push_back(problem_at(path, 1, std::string{path} + " is larger than 1 MiB"));
        else
            problems.push_back(problem_at(path, 0, "it cannot be read"));
        return;
    }
    script::ScriptError error;
    if (!script::compile_check(bytes, path, error)) {
        problems.push_back(problem_at(path, error.line, error.message));
        return;
    }
    ++scripts;
}

/// Writes a list of strings.
///
/// @param[in,out] writer the JSON being written
/// @param lines the strings
void write_strings(json::JsonWriter& writer, std::span<const std::string> lines) {
    writer.begin_array();
    for (const std::string& line : lines)
        writer.string(line);
    writer.end_array();
}

/// Writes one string member when the text is not empty.
///
/// @param[in,out] writer the object
/// @param key the member's name
/// @param text the text
void write_text(json::JsonWriter& writer, std::string_view key, std::string_view text) {
    if (text.empty())
        return;
    writer.key(key);
    writer.string(text);
}

/// Returns a condition as the manifest wrote it: the id, then the range.
///
/// @param condition the condition
/// @return the text
std::string condition_text(const mut::Condition& condition) {
    if (condition.range_text.empty())
        return condition.id;
    return condition.id + " " + condition.range_text;
}

/// Appends the icon problem.
///
/// @param[in,out] problems the problems
/// @param path the icon's path
void refuse_icon(std::vector<std::string>& problems, std::string_view path) {
    problems.push_back(std::string{path} + ": " + std::string{icon_refusal});
}

/// Checks that the icon is a square PNG within the limits.
///
/// @param entries the reader
/// @param paths the package's files
/// @param manifest the manifest
/// @param[in,out] problems the problems
void check_icon(
    const PackageEntries& entries,
    const std::vector<std::string>& paths,
    const brain::Manifest& manifest,
    std::vector<std::string>& problems
) {
    if (manifest.icon.empty())
        return;
    bool held = false;
    for (const std::string& path : paths) {
        if (path == manifest.icon) {
            held = true;
            break;
        }
    }
    if (!held) {
        refuse_icon(problems, manifest.icon);
        return;
    }
    std::vector<uint8_t> bytes;
    bool too_large = false;
    if (!read_limited(
            entries,
            manifest.icon,
            static_cast<std::size_t>(install::oabrain::most_icon_bytes),
            bytes,
            too_large
        )) {
        refuse_icon(problems, manifest.icon);
        return;
    }
    png::Info info{};
    const bool png_ok = png::has_signature(bytes) && png::read_info(bytes, png::Messages{}, &info);
    const bool square = png_ok && info.header.width == info.header.height &&
                        info.header.width >= 1 &&
                        info.header.width <= install::oabrain::most_icon_side;
    if (!square)
        refuse_icon(problems, manifest.icon);
}

/// Reads one network, hashes it, and records it.
///
/// @param entries the reader
/// @param network the manifest's network
/// @param[in,out] problems the problems
/// @param[in,out] total the sizes added so far
/// @param[in,out] over true once the networks together pass 64 MiB
/// @param[in,out] checked receives the network when its bytes were read
void check_network(
    const PackageEntries& entries,
    const brain::Network& network,
    std::vector<std::string>& problems,
    uint64_t& total,
    bool& over,
    BrainChecked& checked
) {
    std::vector<uint8_t> bytes;
    bool too_large = false;
    if (!read_limited(entries, network.file, brain::most_network_bytes, bytes, too_large)) {
        if (too_large) {
            problems.push_back(std::string{network.file} + ": it is larger than 64 MiB");
            over = true;
        } else {
            problems.push_back(std::string{network.file} + ": it is missing");
        }
        return;
    }
    const auto hex = sha256::to_hex(sha256::digest_of(bytes));
    const std::string actual{hex.data(), hex.size()};
    if (actual != network.sha256) {
        problems.push_back(
            network.file + ": its SHA-256 is " + actual + ", not the " + network.sha256 +
            " oabrain.yaml gives"
        );
    }
    const uint64_t size = static_cast<uint64_t>(bytes.size());
    if (total > std::numeric_limits<uint64_t>::max() - size) {
        over = true;
    } else {
        total += size;
        if (total > static_cast<uint64_t>(brain::most_network_bytes))
            over = true;
    }
    NetworkFact fact;
    fact.id = network.id;
    fact.file = network.file;
    fact.sha256 = actual;
    fact.size = size;
    checked.networks.push_back(std::move(fact));
}

/// Refuses a data table larger than 4 MiB.
///
/// @param entries the reader
/// @param path the table
/// @param[in,out] problems the problems
void check_table(
    const PackageEntries& entries, std::string_view path, std::vector<std::string>& problems
) {
    std::vector<uint8_t> bytes;
    bool too_large = false;
    if (read_limited(entries, path, brain::most_data_bytes, bytes, too_large))
        return;
    if (too_large)
        problems.push_back(std::string{path} + ": it is larger than 4 MiB");
    else
        problems.push_back(std::string{path} + ": it cannot be read");
}

} // namespace

void check_brain_files(
    const PackageEntries& entries,
    const brain::Manifest& manifest,
    std::vector<std::string>& problems,
    BrainChecked& checked
) {
    checked = {};
    std::vector<std::string> paths;
    if (entries.list != nullptr)
        entries.list(entries.context, paths);
    std::sort(paths.begin(), paths.end(), bytes_before);

    bool held_entry = false;
    std::vector<std::string> sources;
    for (const std::string& path : paths) {
        const brain::FileRole role = brain::file_role(path, manifest);
        if (role == brain::FileRole::outside)
            problems.push_back(path + ": outside the files a brain may hold");
        if (path == manifest.entry)
            held_entry = true;
        if (role == brain::FileRole::entry || role == brain::FileRole::module)
            sources.push_back(path);
        if (role == brain::FileRole::data)
            checked.data.push_back(path);
    }
    if (!held_entry)
        problems.push_back(manifest.entry + ": it is missing");
    for (const std::string& path : sources)
        compile_one(entries, path, problems, checked.scripts);

    uint64_t total = 0;
    bool over = false;
    for (const brain::Network& network : manifest.networks)
        check_network(entries, network, problems, total, over, checked);
    checked.network_bytes = total;
    if (over)
        problems.push_back("networks: they take more than 64 MiB");

    for (const std::string& path : checked.data)
        check_table(entries, path, problems);
    check_icon(entries, paths, manifest, problems);
}

void describe_oabrain(
    const install::Package& package, json::JsonWriter& writer, const BrainChecked& checked
) {
    std::string manifest_path;
    for (const install::PackagedFile& file : package.files) {
        if (file.entry >= package.directory.entries.size())
            continue;
        if (package.directory.entries[file.entry].directory)
            continue;
        if (same_ascii(file.name, brain::manifest_file)) {
            manifest_path = file.name;
            break;
        }
    }
    if (manifest_path.empty())
        return;
    const PackageEntries entries = entries_of(package);
    std::vector<uint8_t> bytes;
    bool too_large = false;
    if (!read_limited(entries, manifest_path, oamod::max_input_bytes, bytes, too_large) ||
        too_large)
        return;
    brain::Manifest manifest;
    std::vector<brain::Problem> problems;
    if (!brain::read_manifest(bytes, manifest, problems))
        return;

    write_text(writer, "summary", brain::summary_of(manifest));
    write_text(writer, "description", manifest.description);
    write_text(writer, "homepage", manifest.homepage);
    if (!manifest.tags.empty()) {
        writer.key("tags");
        write_strings(writer, manifest.tags);
    }
    write_text(writer, "author", manifest.author.name);
    if (!manifest.requires_engine.empty() || !manifest.requires_expansions.empty()) {
        writer.key("requires");
        writer.begin_object();
        write_text(writer, "engine", manifest.requires_engine);
        if (!manifest.requires_expansions.empty()) {
            writer.key("expansions");
            write_strings(writer, manifest.requires_expansions);
        }
        writer.end_object();
    }
    if (!manifest.works_with.empty()) {
        writer.key("works_with");
        writer.begin_array();
        for (const mut::Condition& condition : manifest.works_with)
            writer.string(condition_text(condition));
        writer.end_array();
    }
    if (!manifest.levels.empty()) {
        writer.key("levels");
        writer.begin_array();
        for (const brain::Level& level : manifest.levels) {
            writer.begin_object();
            writer.key("id");
            writer.string(level.id);
            writer.key("name");
            writer.string(level.name);
            writer.key("difficulty");
            writer.string(brain::difficulty_name(level.difficulty));
            writer.end_object();
        }
        writer.end_array();
    }
    writer.key("full_awareness");
    writer.boolean(manifest.full_awareness);
    if (!checked.networks.empty()) {
        writer.key("networks");
        writer.begin_array();
        for (const NetworkFact& fact : checked.networks) {
            writer.begin_object();
            writer.key("id");
            writer.string(fact.id);
            writer.key("file");
            writer.string(fact.file);
            writer.key("sha256");
            writer.string(fact.sha256);
            writer.key("size");
            writer.integer(static_cast<int64_t>(fact.size));
            writer.end_object();
        }
        writer.end_array();
    }
    writer.key("network_bytes");
    writer.integer(static_cast<int64_t>(checked.network_bytes));
    if (!manifest.options.empty()) {
        writer.key("options");
        writer.begin_array();
        for (const mut::Option& option : manifest.options)
            write_option_facts(writer, option);
        writer.end_array();
    }
    write_icon_facts(package, manifest.icon, install::oabrain::most_icon_bytes, writer);
    write_text(writer, "entry", manifest.entry);
    writer.key("scripts");
    writer.integer(static_cast<int64_t>(checked.scripts));
    if (!checked.data.empty()) {
        writer.key("data");
        write_strings(writer, checked.data);
    }
}

} // namespace oa::tool
