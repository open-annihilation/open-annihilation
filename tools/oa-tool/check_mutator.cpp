// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Checks a mutator's Lua by compiling it, and its rule files by parsing
// them. Neither is run. The facts are the ones a catalogue entry copies.

#include "check_mutator.hpp"

#include "oa/app/package_install/oamut.hpp"
#include "oa/base/sha256.hpp"
#include "oa/data/script_host/script_host.hpp"
#include "oa/formats/oamod.hpp"
#include "oa/formats/oamut/manifest.hpp"
#include "oa/formats/png.hpp"
#include "oa/formats/soft_mutator.hpp"
#include "oa/formats/zip/stream.hpp"
#include "oa/platform/files.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::tool {
namespace {

namespace install = oa::app::package_install;
namespace json = oa::formats::json;
namespace mut = oa::formats::oamut;
namespace oamod = oa::formats::oamod;
namespace png = oa::formats::png;
namespace script = oa::data::script_host;
namespace sha256 = oa::base::sha256;
namespace sm = oa::formats::soft_mutator;
namespace zip = oa::formats::zip;

/// The most bytes of one Lua source this check reads.
constexpr std::size_t most_script_bytes = script::max_source_bytes;

/// The most bytes of one rule file this check reads.
constexpr std::size_t most_rule_bytes = std::size_t{4} << 20;

/// A file opened for a positioned read.
struct FilePos {
    std::FILE* stream{};
    oa::platform::Files files{};

    FilePos() = default;
    FilePos(const FilePos&) = delete;
    FilePos& operator=(const FilePos&) = delete;

    /// Closes the file.
    ~FilePos() {
        if (stream != nullptr)
            std::fclose(stream);
    }

    /// Returns the hooks a streamed zip read uses.
    ///
    /// @return the hooks; they read through this object, which must outlive them
    [[nodiscard]] zip::SourceHooks source() { return zip::SourceHooks{this, &read_at}; }

    /// Reads bytes at an offset.
    ///
    /// @param context the FilePos
    /// @param offset where the bytes start
    /// @param bytes where they are written
    /// @return true when every byte was read
    static bool read_at(void* context, uint64_t offset, std::span<uint8_t> bytes) {
        auto* file = static_cast<FilePos*>(context);
        if (file->stream == nullptr ||
            offset > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
            return false;
        auto* handle = reinterpret_cast<oa::platform::FileHandle*>(file->stream);
        if (file->files.seek(
                nullptr, handle, static_cast<int64_t>(offset), oa::platform::SeekOrigin::begin
            ) != 0)
            return false;
        std::size_t done = 0;
        while (done < bytes.size()) {
            const std::size_t got =
                std::fread(bytes.data() + done, 1, bytes.size() - done, file->stream);
            if (got == 0)
                return false;
            done += got;
        }
        return true;
    }
};

/// Returns a text with its ASCII letters lowered.
///
/// @param text the text
/// @return the text
std::string folded(std::string_view text) {
    std::string lowered(text);
    for (char& character : lowered) {
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character - 'A' + 'a');
    }
    return lowered;
}

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

/// Tells whether a path is a Lua file under `lua/`.
///
/// @param path the package path
/// @return true when it is `lua/**/*.lua`, compared without ASCII case
bool lua_module(std::string_view path) {
    const std::string name = folded(path);
    return name.starts_with("lua/") && name.ends_with(".lua") &&
           name.size() > std::string("lua/").size();
}

/// Tells whether a path is a rule file under `rules/`.
///
/// @param path the package path
/// @return true when it is `rules/**/*.sm`, compared without ASCII case
bool rule_file(std::string_view path) {
    const std::string name = folded(path);
    return name.starts_with("rules/") && name.ends_with(".sm") &&
           name.size() > std::string("rules/").size();
}

/// Appends every file the installer would unpack.
///
/// Paths are the ones `package.files` names, below a stripped top folder.
/// A directory, and anything the installer leaves out, is skipped.
///
/// @param context the package
/// @param[out] paths the file paths
void list_package(void* context, std::vector<std::string>& paths) {
    const auto* package = static_cast<const install::Package*>(context);
    if (package == nullptr)
        return;
    for (const install::PackagedFile& file : package->files) {
        if (file.entry >= package->directory.entries.size())
            continue;
        if (package->directory.entries[file.entry].directory)
            continue;
        if (file.name.empty() || file.name.back() == '/')
            continue;
        paths.push_back(file.name);
    }
}

/// Copies one file out of an opened package.
///
/// @param context the package
/// @param path the file's path
/// @param limit the most bytes it may hold
/// @param[out] bytes the file; empty when it was not read
/// @param[out] too_large true when the file is larger than limit
/// @return true when the file was read
bool read_package(
    void* context,
    std::string_view path,
    std::size_t limit,
    std::vector<uint8_t>& bytes,
    bool& too_large
) {
    bytes.clear();
    too_large = false;
    const auto* package = static_cast<const install::Package*>(context);
    if (package == nullptr)
        return false;
    const zip::StreamEntry* found = nullptr;
    for (const install::PackagedFile& file : package->files) {
        if (file.name != path || file.entry >= package->directory.entries.size())
            continue;
        const zip::StreamEntry& entry = package->directory.entries[file.entry];
        if (!entry.directory) {
            found = &entry;
            break;
        }
    }
    if (found == nullptr)
        return false;
    if (found->bytes > limit) {
        too_large = true;
        return false;
    }
    FilePos file;
    file.stream = oa::platform::open_file(package->file, "rb");
    if (file.stream == nullptr)
        return false;
    file.files = oa::platform::stdio_files();
    zip::ZipError error{};
    return zip::read_stream_entry(
        file.source(), package->archive_bytes, *found, found->bytes, bytes, error
    );
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

/// Finds a file whose name matches `wanted`, without ASCII case.
///
/// @param paths the package's files
/// @param wanted the name
/// @return the stored path; empty when none matches
std::string path_like(const std::vector<std::string>& paths, std::string_view wanted) {
    for (const std::string& path : paths) {
        if (same_ascii(path, wanted))
            return path;
    }
    return {};
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
    if (!read_limited(entries, path, most_script_bytes, bytes, too_large)) {
        if (too_large)
            problems.push_back(problem_at(path, 1, std::string{path} + " is larger than 1 MiB"));
        else
            problems.push_back(problem_at(path, 0, "it cannot be read"));
        return;
    }
    script::ScriptError error;
    if (!script::compile_check(bytes, path, error)) {
        const int64_t line = error.line;
        problems.push_back(problem_at(path, line, error.message));
        return;
    }
    ++scripts;
}

/// Parses one rule file and appends each error.
///
/// @param entries the reader
/// @param path the rule file
/// @param[in,out] problems the problems
void parse_one(
    const PackageEntries& entries, std::string_view path, std::vector<std::string>& problems
) {
    std::vector<uint8_t> bytes;
    bool too_large = false;
    if (!read_limited(entries, path, most_rule_bytes, bytes, too_large)) {
        if (too_large)
            problems.push_back(problem_at(path, 1, "the rule file is larger than 4 MiB"));
        else
            problems.push_back(problem_at(path, 0, "it cannot be read"));
        return;
    }
    const sm::ParseResult parsed = sm::parse_rules(bytes);
    for (const sm::Diagnostic& diagnostic : parsed.diagnostics) {
        if (diagnostic.severity != sm::Severity::error)
            continue;
        problems.push_back(
            problem_at(path, static_cast<int64_t>(diagnostic.line), diagnostic.message)
        );
    }
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

/// Writes one mutator requirement. `version` is left out when none was written.
///
/// @param[in,out] writer the array the object is written into
/// @param condition the requirement
void write_requirement(json::JsonWriter& writer, const mut::Condition& condition) {
    writer.begin_object();
    writer.key("id");
    writer.string(condition.id);
    if (!condition.range_text.empty()) {
        writer.key("version");
        writer.string(condition.range_text);
    }
    writer.end_object();
}

/// Writes a choice value as a JSON number or a string.
///
/// @param[in,out] writer the array
/// @param value the value
void write_choice_value(json::JsonWriter& writer, const mut::ChoiceValue& value) {
    if (value.number)
        writer.raw(oamod::canonical_number_text(value.number_value));
    else
        writer.string(value.text);
}

} // namespace

/// Writes one option's summary.
///
/// @param[in,out] writer the array
/// @param option the option
void write_option_facts(json::JsonWriter& writer, const mut::Option& option) {
    writer.begin_object();
    writer.key("id");
    writer.string(option.id);
    writer.key("label");
    writer.string(option.label);
    writer.key("type");
    writer.string(
        option.type == mut::OptionType::toggle   ? "toggle"
        : option.type == mut::OptionType::choice ? "choice"
                                                 : "number"
    );
    writer.key("default");
    if (option.type == mut::OptionType::toggle) {
        writer.boolean(option.toggle_default);
    } else if (option.type == mut::OptionType::choice) {
        if (option.default_index < option.values.size())
            write_choice_value(writer, option.values[option.default_index]);
        else
            writer.null();
        writer.key("values");
        writer.begin_array();
        for (const mut::ChoiceValue& value : option.values)
            write_choice_value(writer, value);
        writer.end_array();
        if (!option.value_labels.empty()) {
            writer.key("labels");
            write_strings(writer, option.value_labels);
        }
    } else {
        writer.raw(oamod::canonical_number_text(option.number_default));
        writer.key("min");
        writer.raw(oamod::canonical_number_text(option.minimum));
        writer.key("max");
        writer.raw(oamod::canonical_number_text(option.maximum));
        writer.key("step");
        writer.raw(oamod::canonical_number_text(option.step));
    }
    writer.end_object();
}

/// Writes the icon's path, digest, size and pixel size.
///
/// @param package the package
/// @param path the icon's path in the manifest
/// @param size_limit the most bytes of that file
/// @param[in,out] writer the object
void write_icon_facts(
    const install::Package& package,
    std::string_view path,
    uint64_t size_limit,
    json::JsonWriter& writer
) {
    if (path.empty())
        return;
    std::string stored;
    const zip::StreamEntry* found = nullptr;
    for (const install::PackagedFile& file : package.files) {
        if (file.entry >= package.directory.entries.size())
            continue;
        const zip::StreamEntry& entry = package.directory.entries[file.entry];
        if (!entry.directory && same_ascii(file.name, path)) {
            stored = file.name;
            found = &entry;
            break;
        }
    }
    if (found == nullptr || found->bytes > size_limit)
        return;
    PackageEntries entries = entries_of(package);
    std::vector<uint8_t> bytes;
    bool too_large = false;
    if (!read_limited(entries, stored, static_cast<std::size_t>(size_limit), bytes, too_large))
        return;
    png::Info info{};
    if (!png::has_signature(bytes) || !png::read_info(bytes, png::Messages{}, &info))
        return;
    const sha256::Digest digest = sha256::digest_of(bytes);
    const auto hex = sha256::to_hex(digest);
    writer.key("icon");
    writer.begin_object();
    writer.key("path");
    writer.string(path);
    writer.key("sha256");
    writer.string(std::string_view{hex.data(), hex.size()});
    writer.key("size");
    writer.integer(static_cast<int64_t>(bytes.size()));
    writer.key("width");
    writer.integer(static_cast<int64_t>(info.header.width));
    writer.key("height");
    writer.integer(static_cast<int64_t>(info.header.height));
    writer.end_object();
}

PackageEntries entries_of(const install::Package& package) {
    return PackageEntries{const_cast<install::Package*>(&package), &list_package, &read_package};
}

void check_mutator_scripts_and_rules(
    const PackageEntries& entries, std::vector<std::string>& problems, MutatorChecked& checked
) {
    checked = {};
    std::vector<std::string> paths;
    if (entries.list != nullptr)
        entries.list(entries.context, paths);

    std::string entry;
    const std::string manifest_path = path_like(paths, mut::manifest_file);
    if (!manifest_path.empty()) {
        std::vector<uint8_t> bytes;
        bool too_large = false;
        if (read_limited(entries, manifest_path, oamod::max_input_bytes, bytes, too_large) &&
            !too_large) {
            mut::Manifest manifest;
            std::vector<mut::Problem> manifest_problems;
            if (mut::read_manifest(bytes, manifest, manifest_problems))
                entry = manifest.entry;
        }
    }

    std::vector<std::string> sources;
    for (const std::string& path : paths) {
        if (lua_module(path))
            sources.push_back(path);
    }
    if (!entry.empty()) {
        const std::string stored = path_like(paths, entry);
        bool listed = false;
        for (const std::string& path : sources) {
            if (same_ascii(path, stored.empty() ? entry : stored))
                listed = true;
        }
        if (!listed && !stored.empty())
            sources.push_back(stored);
    }
    std::sort(sources.begin(), sources.end(), bytes_before);
    for (const std::string& path : sources)
        compile_one(entries, path, problems, checked.scripts);

    for (const std::string& path : paths) {
        if (rule_file(path))
            checked.rules.push_back(path);
    }
    std::sort(checked.rules.begin(), checked.rules.end(), bytes_before);
    for (const std::string& path : checked.rules)
        parse_one(entries, path, problems);
}

void describe_oamut(
    const install::Package& package, json::JsonWriter& writer, const MutatorChecked& checked
) {
    std::string manifest_path;
    for (const install::PackagedFile& file : package.files) {
        if (file.entry >= package.directory.entries.size())
            continue;
        if (package.directory.entries[file.entry].directory)
            continue;
        if (same_ascii(file.name, mut::manifest_file)) {
            manifest_path = file.name;
            break;
        }
    }
    if (manifest_path.empty())
        return;
    PackageEntries entries = entries_of(package);
    std::vector<uint8_t> bytes;
    bool too_large = false;
    if (!read_limited(entries, manifest_path, oamod::max_input_bytes, bytes, too_large) ||
        too_large)
        return;
    mut::Manifest manifest;
    std::vector<mut::Problem> problems;
    if (!mut::read_manifest(bytes, manifest, problems))
        return;

    write_text(writer, "summary", mut::summary_of(manifest));
    write_text(writer, "description", manifest.description);
    write_text(writer, "homepage", manifest.homepage);
    if (!manifest.tags.empty()) {
        writer.key("tags");
        write_strings(writer, manifest.tags);
    }
    write_text(writer, "author", manifest.author.name);
    if (!manifest.requires_engine.empty() || !manifest.requires_mutators.empty()) {
        writer.key("requires");
        writer.begin_object();
        write_text(writer, "engine", manifest.requires_engine);
        if (!manifest.requires_mutators.empty()) {
            writer.key("mutators");
            writer.begin_array();
            for (const mut::Condition& condition : manifest.requires_mutators)
                write_requirement(writer, condition);
            writer.end_array();
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
    if (!manifest.conflicts_with.empty()) {
        writer.key("conflicts_with");
        writer.begin_array();
        for (const mut::Condition& condition : manifest.conflicts_with)
            write_requirement(writer, condition);
        writer.end_array();
    }
    if (!manifest.layers.empty()) {
        writer.key("layers");
        writer.begin_array();
        for (const mut::Layer layer : manifest.layers)
            writer.string(mut::layer_name(layer));
        writer.end_array();
    }
    // Absent stays absent, so a publisher does not invent a clash mode.
    if (manifest.clashes.has_value())
        write_text(writer, "clashes", mut::clash_mode_name(*manifest.clashes));
    if (!manifest.options.empty()) {
        writer.key("options");
        writer.begin_array();
        for (const mut::Option& option : manifest.options)
            write_option_facts(writer, option);
        writer.end_array();
    }
    write_icon_facts(package, manifest.icon, install::oamut::most_icon_bytes, writer);
    write_text(writer, "entry", manifest.entry);
    writer.key("scripts");
    writer.integer(static_cast<int64_t>(checked.scripts));
    if (!checked.rules.empty()) {
        writer.key("rules");
        write_strings(writer, checked.rules);
    }
    if (!manifest.legacy.empty()) {
        writer.key("legacy");
        writer.begin_array();
        for (const mut::Legacy& record : manifest.legacy) {
            writer.begin_object();
            writer.key("name");
            writer.string(record.name);
            writer.key("prefix");
            writer.string(record.prefix);
            writer.key("compatibility");
            write_strings(writer, record.compatibility);
            writer.key("files");
            write_strings(writer, record.files);
            writer.end_object();
        }
        writer.end_array();
    }
}

} // namespace oa::tool
