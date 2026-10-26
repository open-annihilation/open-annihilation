// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// check: the installer's own reading of a package, then the facts a
// catalogue lists, as text or as one JSON object. The file is hashed in
// pieces. A map pack is also checked against the game and the mods named
// for the run. Nothing else here decides a package is installable except
// open_package.

#include "check.hpp"

#include "arguments.hpp"
#include "check_brain.hpp"
#include "check_keys.hpp"
#include "check_map.hpp"
#include "check_mutator.hpp"
#include "command.hpp"

#include "oa/app/package_install/oamod.hpp"
#include "oa/base/sha256.hpp"
#include "oa/data/key_library/library.hpp"
#include "oa/data/content_bundles.hpp"
#include "oa/data/content_bundles/expansion_package.hpp"
#include "oa/data/languages/language_pack.hpp"
#include "oa/data/map_pack/manifest.hpp"
#include "oa/data/mod_profile.hpp"
#include "oa/data/mod_profile/registry.hpp"
#include "oa/formats/oabrain/manifest.hpp"
#include "oa/formats/oamod.hpp"
#include "oa/formats/zip/stream.hpp"
#include "oa/platform/files.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <optional>
#include <ostream>
#include <random>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace oa::tool {
namespace {

namespace fs = std::filesystem;
namespace install = oa::app::package_install;
namespace json = oa::formats::json;
namespace sha256 = oa::base::sha256;
namespace zip = oa::formats::zip;
namespace mod_profile = oa::data::mod_profile;
namespace oamod_format = oa::formats::oamod;

/// The catalogue report's check value: the shape of this object's keys.
constexpr int64_t check_format = 1;

/// Bytes hashed from the package file at a time.
constexpr std::size_t read_piece = std::size_t{1} << 20;

/// One hack turned on, in the registry's order.
struct HackOn {
    std::string id{};
    mod_profile::registry::Scope scope{};
    bool implemented{};
};

/// The fields of a language manifest this report writes.
struct LanguageFields {
    bool read{}; ///< the manifest's tree was read
    std::string tag{};
    std::string name{};
    std::string english_name{};
    std::string word{};
    std::string version{};
    std::string text_needs{};
    std::optional<std::vector<std::string>> locales{};
    std::optional<std::vector<std::string>> fallbacks{};
    std::optional<bool> unicode{};
    std::optional<int64_t> revision{}; ///< packaging.revision, when it is a whole number
    /// True when the manifest was read, so the game lists below are filled in.
    bool games_known{false};
    /// Base games the pack's text is for. Every base game when the key is omitted.
    std::vector<std::string> supported_games{};
    /// Expansions that must be on. Empty when the manifest names none.
    std::vector<std::string> required_expansions{};
};

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

/// The size and SHA-256 of a file read in pieces.
struct FileHash {
    uint64_t size{};
    sha256::Digest digest{};
};

/// Returns a path's file name in UTF-8.
///
/// @param path the path
/// @return the file name
std::string file_name_of(const fs::path& path) {
    const auto text = path.filename().generic_u8string();
    return {text.begin(), text.end()};
}

/// Returns a path made from UTF-8 text.
///
/// @param text the text
/// @return the path
fs::path path_of(std::string_view text) {
    return fs::path(std::u8string(text.begin(), text.end()));
}

/// Opens a file for reading.
///
/// @param path the file
/// @param[out] file receives the open file
/// @return true when it opened
bool open_read(const fs::path& path, FilePos& file) {
    file.stream = oa::platform::open_file(path, "rb");
    if (file.stream == nullptr)
        return false;
    file.files = oa::platform::stdio_files();
    return true;
}

/// Hashes a whole file in read_piece reads.
///
/// @param path the file
/// @return its size and SHA-256; nothing when it cannot be read
std::optional<FileHash> hash_file(const fs::path& path) {
    FilePos file;
    if (!open_read(path, file))
        return std::nullopt;
    sha256::Hasher hasher{};
    std::vector<uint8_t> buffer(read_piece);
    uint64_t size = 0;
    while (true) {
        const std::size_t got = std::fread(buffer.data(), 1, buffer.size(), file.stream);
        if (got > 0) {
            sha256::update(hasher, std::span<const uint8_t>(buffer.data(), got));
            size += got;
        }
        if (got < buffer.size()) {
            if (std::ferror(file.stream) != 0)
                return std::nullopt;
            break;
        }
    }
    return FileHash{size, sha256::finish(hasher)};
}

/// Writes a digest as 64 lower-case hex digits.
///
/// @param digest the digest
/// @return the digits
std::string hex_text(const sha256::Digest& digest) {
    const auto hex = sha256::to_hex(digest);
    return {hex.data(), hex.size()};
}

/// Reads a package's manifest from the file, a piece at a time.
///
/// @param package the package
/// @return the manifest's bytes; empty when they cannot be read
std::vector<uint8_t> read_packaged_manifest(const install::Package& package) {
    std::vector<uint8_t> bytes;
    if (package.kind == nullptr || package.manifest_entry >= package.directory.entries.size())
        return bytes;
    FilePos file;
    if (!open_read(package.file, file))
        return bytes;
    zip::ZipError error{};
    if (!zip::read_stream_entry(
            file.source(),
            package.archive_bytes,
            package.directory.entries[package.manifest_entry],
            package.kind->manifest_most_bytes,
            bytes,
            error
        ))
        bytes.clear();
    return bytes;
}

/// Returns the sentence the installer shows for a refusal.
///
/// @param kind the package's kind; null when the name is not a kind
/// @param problem the refusal
/// @return the sentence
std::string refusal_sentence(const install::PackageKind* kind, const install::Problem& problem) {
    std::string text = kind != nullptr ? install::refusal_text(*kind, problem)
                                       : install::oamod::refusal_text(problem);
    if (text.empty())
        text = problem.detail.empty() ? std::string("It cannot be read.") : problem.detail;
    return text;
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

/// Writes a string list when the manifest has the key.
///
/// @param[in,out] writer the object
/// @param key the member's name
/// @param lines the list; nothing when the key is absent
void write_list(
    json::JsonWriter& writer,
    std::string_view key,
    const std::optional<std::vector<std::string>>& lines
) {
    if (!lines)
        return;
    writer.key(key);
    write_strings(writer, *lines);
}

/// Returns a mapping entry's string, when it is one.
///
/// @param node the entry; null for none
/// @return the text; empty when it is not a string
std::string string_of(const oamod_format::Node* node) {
    if (node == nullptr || node->kind != oamod_format::NodeKind::string)
        return {};
    return node->text;
}

/// Returns a sequence's strings, when the entry is a sequence.
///
/// @param node the entry; null for none
/// @return the strings; nothing when the entry is absent or not a sequence
std::optional<std::vector<std::string>> strings_of(const oamod_format::Node* node) {
    if (node == nullptr || node->kind != oamod_format::NodeKind::sequence)
        return std::nullopt;
    std::vector<std::string> lines;
    for (const oamod_format::Node& item : node->children)
        if (item.kind == oamod_format::NodeKind::string)
            lines.push_back(item.text);
    return lines;
}

/// Reads the language fields a catalogue lists from a manifest's tree.
///
/// @param manifest the language.yaml bytes
/// @return the fields; read is false when the text is not a manifest
LanguageFields read_language(std::span<const uint8_t> manifest) {
    LanguageFields fields{};
    oamod_format::Node root;
    oamod_format::ReadError error{};
    if (!oamod_format::read_document(manifest, root, error))
        return fields;
    fields.read = true;
    fields.tag = string_of(oamod_format::find_entry(root, "tag"));
    fields.name = string_of(oamod_format::find_entry(root, "name"));
    fields.english_name = string_of(oamod_format::find_entry(root, "english-name"));
    fields.word = string_of(oamod_format::find_entry(root, "word"));
    fields.version = string_of(oamod_format::find_entry(root, "version"));
    fields.locales = strings_of(oamod_format::find_entry(root, "locales"));
    fields.fallbacks = strings_of(oamod_format::find_entry(root, "fallbacks"));
    if (const oamod_format::Node* text = oamod_format::find_entry(root, "text"))
        fields.text_needs = string_of(oamod_format::find_entry(*text, "needs"));
    if (const oamod_format::Node* unicode = oamod_format::find_entry(root, "unicode");
        unicode != nullptr && unicode->kind == oamod_format::NodeKind::boolean)
        fields.unicode = unicode->boolean;
    if (const oamod_format::Node* packaging = oamod_format::find_entry(root, "packaging")) {
        const oamod_format::Node* revision = oamod_format::find_entry(*packaging, "revision");
        int64_t value = 0;
        if (revision != nullptr && revision->kind == oamod_format::NodeKind::number &&
            oamod_format::integer_value(revision->number, value))
            fields.revision = value;
    }
    oa::data::languages::PackManifest pack;
    if (oa::data::languages::read_manifest(manifest, pack)) {
        fields.games_known = true;
        fields.supported_games = std::move(pack.supported_games);
        fields.required_expansions = std::move(pack.required_expansions);
    }
    return fields;
}

/// Writes a language pack's id, name, version and revision.
///
/// @param fields the manifest's fields
/// @param[in,out] writer the object
void write_oalang_identity(const LanguageFields& fields, json::JsonWriter& writer) {
    write_text(writer, "id", fields.tag);
    write_text(writer, "name", fields.name);
    write_text(writer, "version", fields.version);
    writer.key("revision");
    if (fields.revision)
        writer.integer(*fields.revision);
    else
        writer.null();
}

/// Writes supportedGames and requiredExpansions, including an empty expansion list.
///
/// @param[in,out] writer the object
/// @param games the base games
/// @param expansions the expansions
void write_game_lists(
    json::JsonWriter& writer,
    const std::vector<std::string>& games,
    const std::vector<std::string>& expansions
) {
    writer.key("supportedGames");
    write_strings(writer, games);
    writer.key("requiredExpansions");
    write_strings(writer, expansions);
}

/// Writes a language pack's language object.
///
/// @param fields the manifest's fields
/// @param[in,out] writer the object
void write_oalang_language(const LanguageFields& fields, json::JsonWriter& writer) {
    writer.key("language");
    writer.begin_object();
    write_text(writer, "tag", fields.tag);
    write_text(writer, "name", fields.name);
    write_text(writer, "english-name", fields.english_name);
    write_text(writer, "word", fields.word);
    write_list(writer, "locales", fields.locales);
    write_list(writer, "fallbacks", fields.fallbacks);
    write_text(writer, "text-needs", fields.text_needs);
    if (fields.unicode) {
        writer.key("unicode");
        writer.boolean(*fields.unicode);
    }
    writer.end_object();
}

/// Returns the hacks a resolved profile turns on, in the registry's order.
///
/// @param effective the effective profile
/// @return one entry per hack that is on
std::vector<HackOn> hacks_on(const mod_profile::Value& effective) {
    const std::vector<mod_profile::HackState> states = mod_profile::hack_states(effective);
    const std::span<const mod_profile::registry::Entry* const> hacks =
        mod_profile::standard_hacks();
    std::vector<HackOn> on;
    const std::size_t count = states.size() < hacks.size() ? states.size() : hacks.size();
    for (std::size_t index = 0; index < count; ++index) {
        if (!states[index].on || hacks[index] == nullptr)
            continue;
        on.push_back(
            HackOn{std::string(hacks[index]->id), hacks[index]->scope, hacks[index]->implemented}
        );
    }
    return on;
}

/// Resolves a mod's profile as a catalogue compares it: no settings, and a
/// hack this build does not carry out yet accepted so the hashes still exist.
///
/// @param package the package
/// @return the resolution; nothing when the profile cannot be read or resolved
std::optional<mod_profile::Resolution> catalogue_resolution(const install::Package& package) {
    const std::vector<uint8_t> manifest = read_packaged_manifest(package);
    if (manifest.empty())
        return std::nullopt;
    mod_profile::ResolveOptions options{};
    options.accept_unimplemented_hacks = true;
    const std::string source = package.file_name + "/oamod.yaml";
    mod_profile::ResolveResult resolved = mod_profile::resolve_profile(manifest, source, options);
    if (!resolved.resolution)
        return std::nullopt;
    return std::move(*resolved.resolution);
}

/// The note for a manifest that still writes requires.base.
constexpr std::string_view legacy_base_note =
    "requires.base: ta-3.1c reads as supportedGames: [totala]; write supportedGames instead";

/// The last part of a path, after / or \.
///
/// @param path the path
/// @return the file name
std::string_view last_path_name(std::string_view path) {
    const std::size_t slash = path.find_last_of("/\\");
    if (slash == std::string_view::npos)
        return path;
    return path.substr(slash + 1);
}

/// Compares two file names, ASCII case folded.
///
/// @param left one name
/// @param right the other
/// @return true when they are the same name
bool same_file_name(std::string_view left, std::string_view right) {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        unsigned char first = static_cast<unsigned char>(left[index]);
        unsigned char second = static_cast<unsigned char>(right[index]);
        if (first >= 'A' && first <= 'Z')
            first = static_cast<unsigned char>(first - 'A' + 'a');
        if (second >= 'A' && second <= 'Z')
            second = static_cast<unsigned char>(second - 'A' + 'a');
        if (first != second)
            return false;
    }
    return true;
}

/// The expansion whose archive this name is, compared on the file name only.
///
/// @param written the name as the manifest writes it
/// @return the expansion's id; empty when the name is not an expansion archive
std::string expansion_for_archive(std::string_view written) {
    const std::string_view name = last_path_name(written);
    if (name.empty())
        return {};
    namespace bundles = oa::data::content_bundles;
    for (const bundles::Bundle& bundle : bundles::bundles()) {
        if (bundle.kind != bundles::Kind::expansion)
            continue;
        for (const bundles::ContentFile& file : bundle.files) {
            if (same_file_name(last_path_name(file.copy_name), name))
                return std::string{bundle.id};
            for (const std::string_view source : file.source_names) {
                if (same_file_name(last_path_name(source), name))
                    return std::string{bundle.id};
            }
        }
    }
    return {};
}

/// Appends suggestions about the old base key and expansion archives.
///
/// A suggestion stays a warning. It does not refuse a package the installer accepts.
///
/// @param package the opened package
/// @param kind the package's kind
/// @param[in,out] warnings the report's warnings
void append_content_notes(
    const install::Package& package,
    const install::PackageKind& kind,
    std::vector<std::string>& warnings
) {
    if (kind.name == "expansion")
        return;
    if (kind.name == "oamod") {
        const std::optional<mod_profile::Resolution> resolved = catalogue_resolution(package);
        if (!resolved)
            return;
        const mod_profile::Value* required =
            mod_profile::find_member(resolved->effective, "requires");
        const mod_profile::Value* base =
            required != nullptr ? mod_profile::find_member(*required, "base") : nullptr;
        if (base != nullptr && base->kind == mod_profile::ValueKind::string && !base->text.empty())
            warnings.emplace_back(legacy_base_note);
        for (const std::string& archive : resolved->profile.layout.installation_archives) {
            const std::string expansion = expansion_for_archive(archive);
            if (expansion.empty())
                continue;
            warnings.push_back(
                "layout.installation-archives: " + archive + " is an archive of " + expansion +
                "; declare requiredExpansions: [" + expansion + "] instead"
            );
        }
        return;
    }
    const std::vector<uint8_t> manifest = read_packaged_manifest(package);
    if (kind.name == "oamap") {
        oa::data::map_pack::Manifest read;
        std::vector<oa::data::map_pack::Problem> problems;
        if (oa::data::map_pack::read_manifest(
                manifest, oa::data::map_pack::ManifestUse::package, read, problems
            ) &&
            !read.requires_base.empty())
            warnings.emplace_back(legacy_base_note);
        return;
    }
    if (kind.name == "oalang") {
        oa::data::languages::PackManifest read;
        if (oa::data::languages::read_manifest(manifest, read) && !read.requires_base.empty())
            warnings.emplace_back(legacy_base_note);
    }
}

/// Writes requires.base and requires.engine when the profile has them.
///
/// @param effective the effective profile, whose requires.base is as written
/// @param engine the engine requirement as written; empty when the profile has none
/// @param[in,out] writer the object
void write_requires(
    const mod_profile::Value& effective, std::string_view engine, json::JsonWriter& writer
) {
    const mod_profile::Value* required = mod_profile::find_member(effective, "requires");
    const mod_profile::Value* base =
        required != nullptr ? mod_profile::find_member(*required, "base") : nullptr;
    const bool has_base = base != nullptr && base->kind == mod_profile::ValueKind::string;
    if (!has_base && engine.empty())
        return;
    writer.key("requires");
    writer.begin_object();
    if (has_base)
        write_text(writer, "base", base->text);
    write_text(writer, "engine", engine);
    writer.end_object();
}

/// Writes packaging when the profile has it.
///
/// @param effective the effective profile
/// @param[in,out] writer the object
void write_packaging(const mod_profile::Value& effective, json::JsonWriter& writer) {
    const mod_profile::Value* packaging = mod_profile::find_member(effective, "packaging");
    if (packaging == nullptr || packaging->kind != mod_profile::ValueKind::map)
        return;
    const mod_profile::Value* revision = mod_profile::find_member(*packaging, "revision");
    int64_t revision_number = 0;
    const bool has_revision = revision != nullptr &&
                              revision->kind == mod_profile::ValueKind::number &&
                              oamod_format::integer_value(revision->number, revision_number);
    const mod_profile::Value* date = mod_profile::find_member(*packaging, "date");
    const mod_profile::Value* packager = mod_profile::find_member(*packaging, "packager");
    const bool has_date = date != nullptr && date->kind == mod_profile::ValueKind::string;
    const bool has_packager =
        packager != nullptr && packager->kind == mod_profile::ValueKind::string;
    if (!has_revision && !has_date && !has_packager)
        return;
    writer.key("packaging");
    writer.begin_object();
    if (has_revision) {
        writer.key("revision");
        writer.integer(revision_number);
    }
    if (has_date)
        write_text(writer, "date", date->text);
    if (has_packager)
        write_text(writer, "packager", packager->text);
    writer.end_object();
}

/// Writes one JSON value again, for a text line's array or number.
///
/// @param[in,out] writer the JSON being written
/// @param value the value
void write_json_value(json::JsonWriter& writer, const json::Json& value) {
    switch (value.type()) {
    case json::JsonType::null:
        writer.null();
        break;
    case json::JsonType::boolean:
        writer.boolean(value.boolean().value_or(false));
        break;
    case json::JsonType::number:
        if (const std::string* text = value.number_text())
            writer.raw(*text);
        else
            writer.null();
        break;
    case json::JsonType::string:
        writer.string(value.string() != nullptr ? *value.string() : std::string_view{});
        break;
    case json::JsonType::array:
        writer.begin_array();
        for (const json::Json& item : value.elements())
            write_json_value(writer, item);
        writer.end_array();
        break;
    case json::JsonType::object:
        writer.begin_object();
        {
            const std::span<const std::string> names = value.names();
            const std::span<const json::Json> values = value.values();
            const std::size_t count = names.size() < values.size() ? names.size() : values.size();
            for (std::size_t index = 0; index < count; ++index) {
                writer.key(names[index]);
                write_json_value(writer, values[index]);
            }
        }
        writer.end_object();
        break;
    }
}

/// Prints one fact, or each member of an object, as `key: value`.
///
/// @param[in,out] out where the lines are written
/// @param key the fact's name; empty at the report's top
/// @param value the fact
void print_fact(std::ostream& out, std::string_view key, const json::Json& value) {
    if (value.type() == json::JsonType::object) {
        const std::span<const std::string> names = value.names();
        const std::span<const json::Json> values = value.values();
        const std::size_t count = names.size() < values.size() ? names.size() : values.size();
        for (std::size_t index = 0; index < count; ++index) {
            std::string nested(key);
            if (!nested.empty())
                nested.push_back('.');
            nested += names[index];
            print_fact(out, nested, values[index]);
        }
        return;
    }
    out << key << ": ";
    if (value.type() == json::JsonType::string && value.string() != nullptr)
        out << *value.string();
    else if (value.type() == json::JsonType::boolean)
        out << (value.boolean().value_or(false) ? "true" : "false");
    else if (value.type() == json::JsonType::null)
        out << "null";
    else {
        json::JsonWriter writer;
        write_json_value(writer, value);
        out << writer.text();
    }
    out << '\n';
}

/// Prints a usage failure the way the dispatcher does.
///
/// @param output where it is printed
/// @param problem what is wrong
void report_usage(Output& output, std::string_view problem) {
    output.err << "oa-tool check: " << problem << '\n';
    output.err << "run 'oa-tool help check'\n";
}

/// Tells whether one `--mod` value names a folder, and a key when it has `=`.
///
/// The split is the last `=`. A value with no `=` is a folder. An empty
/// folder, or an empty key after that `=`, is not a target.
///
/// @param text the option's value
/// @return true when the folder is not empty, and the key is not empty if `=` was written
bool mod_value_ok(std::string_view text) {
    const auto split = text.rfind('=');
    if (split == std::string_view::npos)
        return !text.empty();
    return split != 0 && split + 1 < text.size();
}

/// Splits one `--mod` value into a folder and an optional key.
///
/// @param text a value mod_value_ok accepted
/// @return the folder, and the key after the last `=`; the key is empty when there is no `=`
MapTarget mod_target(std::string_view text) {
    const auto split = text.rfind('=');
    if (split == std::string_view::npos)
        return MapTarget{path_of(text), {}};
    return MapTarget{path_of(text.substr(0, split)), std::string(text.substr(split + 1))};
}

/// Copies an object's members onto a writer, in the object's order.
///
/// @param[in,out] writer the object the members are written into
/// @param text a JSON object
void copy_object_members(json::JsonWriter& writer, std::string_view text) {
    json::JsonError error{};
    const std::optional<json::Json> parsed = json::parse_json(text, error);
    if (!parsed || parsed->type() != json::JsonType::object)
        return;
    const std::span<const std::string> names = parsed->names();
    const std::span<const json::Json> values = parsed->values();
    const std::size_t count = names.size() < values.size() ? names.size() : values.size();
    for (std::size_t index = 0; index < count; ++index) {
        writer.key(names[index]);
        write_json_value(writer, values[index]);
    }
}

/// Writes an expansion's extends, needs and author after the shared facts.
///
/// @param package the opened package
/// @param[in,out] writer the object
void describe_expansion(const install::Package& package, json::JsonWriter& writer) {
    namespace bundles = oa::data::content_bundles;
    const std::vector<uint8_t> bytes = read_packaged_manifest(package);
    bundles::ExpansionManifest manifest;
    if (!bundles::read_expansion_manifest(bytes, manifest, nullptr))
        return;
    writer.key("extends");
    write_strings(writer, manifest.extends);
    writer.key("needs");
    write_strings(writer, manifest.needs);
    write_text(writer, "author", manifest.author.name);
}

/// Checks an opened brain's files and appends each problem.
///
/// @param package the brain
/// @param[in,out] problems the problems
/// @param[out] checked the script, network and table check
void append_brain_problems(
    const install::Package& package, std::vector<std::string>& problems, BrainChecked& checked
) {
    checked = {};
    const std::vector<uint8_t> bytes = read_packaged_manifest(package);
    oa::formats::oabrain::Manifest manifest;
    std::vector<oa::formats::oabrain::Problem> found;
    if (!oa::formats::oabrain::read_manifest(bytes, manifest, found)) {
        for (const oa::formats::oabrain::Problem& one : found)
            problems.push_back(oa::formats::oabrain::describe(one));
        return;
    }
    check_brain_files(entries_of(package), manifest, problems, checked);
}

/// Checks an opened key layout and appends each problem.
///
/// Warnings stay on `checked`. They replace the package's own warnings:
/// the installer keeps a layout warning as text the player sees, not as a
/// package warning.
///
/// @param package the key layout
/// @param[in,out] problems the problems
/// @param[out] checked the layout's facts and warnings
void append_keys_problems(
    const install::Package& package, std::vector<std::string>& problems, KeysChecked& checked
) {
    checked = {};
    oa::formats::oakeys::Manifest manifest;
    if (!read_oakeys_manifest(entries_of(package), manifest, problems))
        return;
    check_keys_files(entries_of(package), manifest, problems, checked);
}

/// Writes the report and returns the exit code.
///
/// @param file the package's path
/// @param hashed the file's size and SHA-256; nothing when it could not be read
/// @param opened what open_package returned
/// @param as_json print one JSON object
/// @param map_request the game and the mods a map pack is checked against; null
///        when the file is not an accepted map pack
/// @param[in,out] output receives the report
/// @param strict a key layout with a warning is refused; other kinds ignore it
/// @param display_file the file fact, when it is not `file`'s own name
/// @return exit_done when the package is accepted, otherwise exit_failed
int report(
    const fs::path& file,
    const std::optional<FileHash>& hashed,
    const install::PackageResult& opened,
    bool as_json,
    const MapCheckRequest* map_request,
    Output& output,
    bool strict,
    std::string_view display_file
) {
    const install::PackageKind* kind = install::kind_for_file(file);
    const install::Package* package = opened.package ? &*opened.package : nullptr;
    if (package != nullptr && package->kind != nullptr)
        kind = package->kind;

    std::vector<std::string> problems;
    std::vector<std::string> warnings;
    if (package == nullptr) {
        // A key-layout folder is checked before it is a package. Its lines
        // already name each problem, so they are not led by another kind's
        // refusal sentence.
        const bool named_lines =
            !display_file.empty() && kind == nullptr && !opened.problem.lines.empty();
        if (!named_lines)
            problems.push_back(refusal_sentence(kind, opened.problem));
        for (const std::string& line : opened.problem.lines)
            if (!line.empty())
                problems.push_back(line);
    } else {
        warnings = package->warnings;
        if (kind != nullptr)
            append_content_notes(*package, *kind, warnings);
    }

    LanguageFields language{};
    const bool oalang = package != nullptr && kind != nullptr && kind->name == "oalang";
    if (oalang)
        language = read_language(read_packaged_manifest(*package));

    // A map pack's facts and its fit are known before ok is written. Text
    // keeps the fit lines out of the object, so they are not printed as facts.
    json::JsonWriter facts;
    std::string fit_lines;
    const bool oamap = package != nullptr && kind != nullptr && kind->name == "oamap";
    if (oamap) {
        facts.begin_object();
        const std::vector<uint8_t> manifest = read_packaged_manifest(*package);
        std::ostringstream discarded;
        Output describe_output{discarded, output.err};
        const bool described = describe_oamap(manifest, &facts, describe_output, problems);
        if (described && map_request != nullptr) {
            if (as_json) {
                Output fit_output{discarded, output.err};
                const bool fitted = check_map_fit(*map_request, &facts, fit_output, problems);
                static_cast<void>(fitted);
            } else {
                std::ostringstream lines;
                Output fit_output{lines, output.err};
                const bool fitted = check_map_fit(*map_request, nullptr, fit_output, problems);
                static_cast<void>(fitted);
                fit_lines = lines.str();
            }
        }
        facts.end_object();
    }

    const bool oamut = package != nullptr && kind != nullptr && kind->name == "oamut";
    MutatorChecked mutator;
    if (oamut)
        check_mutator_scripts_and_rules(entries_of(*package), problems, mutator);

    const bool oabrain = package != nullptr && kind != nullptr && kind->name == "oabrain";
    BrainChecked brain;
    if (oabrain)
        append_brain_problems(*package, problems, brain);

    const bool oakeys = package != nullptr && kind != nullptr && kind->name == "oakeys";
    KeysChecked keys;
    if (oakeys) {
        append_keys_problems(*package, problems, keys);
        warnings = std::move(keys.warnings);
    }
    // --strict is opt-in and changes only a key layout. Another kind's ok,
    // and every key of its report, stay as they are.
    const bool refused = !problems.empty() || (strict && oakeys && !warnings.empty());

    json::JsonWriter writer;
    writer.begin_object();
    writer.key("check");
    writer.integer(check_format);
    writer.key("ok");
    writer.boolean(!refused);
    writer.key("problems");
    write_strings(writer, problems);
    writer.key("warnings");
    write_strings(writer, warnings);
    writer.key("file");
    writer.string(display_file.empty() ? file_name_of(file) : std::string{display_file});
    if (kind != nullptr)
        write_text(writer, "kind", kind->name);
    if (oalang && language.read) {
        write_oalang_identity(language, writer);
    } else if (package != nullptr && !package->incoming.id.empty()) {
        write_text(writer, "id", package->incoming.id);
        write_text(writer, "name", package->incoming.name);
        write_text(writer, "version", package->incoming.version);
        writer.key("revision");
        if (oamut || oabrain || oakeys)
            writer.null();
        else
            writer.integer(package->incoming.revision);
    }
    if (hashed) {
        writer.key("size");
        writer.integer(static_cast<int64_t>(hashed->size));
        write_text(writer, "sha256", hex_text(hashed->digest));
    }
    if (package != nullptr) {
        writer.key("unpacked_size");
        writer.integer(static_cast<int64_t>(package->unpacked_bytes));
        writer.key("files");
        writer.integer(static_cast<int64_t>(package->files.size()));
        if (kind != nullptr && kind->name == "oamod")
            describe_oamod(*package, writer);
        else if (oalang && language.read) {
            if (language.games_known)
                write_game_lists(writer, language.supported_games, language.required_expansions);
            write_oalang_language(language, writer);
        } else if (oamap)
            copy_object_members(writer, facts.text());
        else if (kind != nullptr && kind->name == "expansion")
            describe_expansion(*package, writer);
        else if (oamut)
            describe_oamut(*package, writer, mutator);
        else if (oabrain)
            describe_oabrain(*package, writer, brain);
        else if (oakeys)
            describe_oakeys(*package, writer, keys);
    }
    writer.end_object();

    if (as_json) {
        output.out << writer.text() << '\n';
    } else {
        json::JsonError error{};
        const std::optional<json::Json> parsed = json::parse_json(writer.text(), error);
        if (!parsed || parsed->type() != json::JsonType::object)
            output.out << writer.text() << '\n';
        else
            print_fact(output.out, {}, *parsed);
        if (!fit_lines.empty())
            output.out << fit_lines;
        output.out << "result: " << (refused ? "refused" : "ok") << '\n';
    }
    return refused ? exit_failed : exit_done;
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

/// Tells whether a folder's top holds oakeys.yaml, without regard to case.
///
/// @param folder the folder
/// @return true when a file there is named oakeys.yaml
bool folder_holds_oakeys(const fs::path& folder) {
    std::error_code listed;
    if (!fs::is_directory(fs::symlink_status(folder, listed)))
        return false;
    try {
        for (const fs::directory_entry& entry : fs::directory_iterator(folder)) {
            std::error_code status_error;
            const fs::file_status status = entry.symlink_status(status_error);
            if (status_error || fs::is_symlink(status) || fs::is_directory(status))
                continue;
            if (same_ascii(file_name_of(entry.path()), oa::formats::oakeys::manifest_file))
                return true;
        }
    } catch (const std::exception&) {
        return false;
    }
    return false;
}

/// The refusal when a key-layout folder also holds another manifest.
///
/// Empty when oakeys.yaml is the only manifest at the top.
///
/// @param folder the folder
/// @return the sentence, or empty
std::string keys_manifest_conflict(const fs::path& folder) {
    static constexpr std::string_view names[] = {
        "oamod.yaml",
        "language.yaml",
        "oamap.yaml",
        "oaexp.yaml",
        "oamut.yaml",
        "oabrain.yaml",
    };
    std::string found;
    int count = 0;
    try {
        for (const fs::directory_entry& entry : fs::directory_iterator(folder)) {
            std::error_code status_error;
            const fs::file_status status = entry.symlink_status(status_error);
            if (status_error || fs::is_symlink(status) || !fs::is_regular_file(status))
                continue;
            const std::string name = file_name_of(entry.path());
            for (const std::string_view known : names) {
                if (!same_ascii(name, known))
                    continue;
                ++count;
                found.assign(known);
                break;
            }
        }
    } catch (const std::exception&) {
        return "the folder cannot be listed";
    }
    if (count == 0)
        return {};
    if (count == 1)
        return "the folder holds both oakeys.yaml and " + found;
    return "the folder holds more than one manifest";
}

/// Removes a path when it is destroyed.
class RemoveFile {
  public:

    /// Takes the path to remove.
    ///
    /// @param path the path
    explicit RemoveFile(fs::path path) : path_(std::move(path)) {}

    RemoveFile(const RemoveFile&) = delete;
    RemoveFile& operator=(const RemoveFile&) = delete;

    /// Removes the path, when it is still held.
    ~RemoveFile() { remove(); }

    /// Removes the path now.
    void remove() {
        if (path_.empty())
            return;
        std::error_code error;
        fs::remove(path_, error);
        path_.clear();
    }

  private:

    fs::path path_{};
};

/// A throwaway .oakeys name. Tests run at the same time, so the name is random.
///
/// @return a path under the temporary directory
fs::path temporary_oakeys() {
    std::random_device device;
    std::ostringstream text;
    text << std::hex << device() << device() << device();
    return fs::temp_directory_path() / ("K44-oakeys-" + text.str() + ".oakeys");
}

/// Checks a folder whose top holds oakeys.yaml.
///
/// A folder with a problem is reported in memory and nothing is left behind.
/// A folder the installer would accept is packed with the key-layout packer,
/// opened, and then the temporary package is deleted. The report's file and
/// hash are the folder's name and the packed bytes.
///
/// @param folder the folder
/// @param as_json print one JSON object
/// @param strict a warning refuses the layout
/// @param[in,out] output receives the report
/// @return the report's exit code
int check_key_folder(const fs::path& folder, bool as_json, bool strict, Output& output) {
    const std::string shown = file_name_of(folder);
    install::PackageResult refused;
    const std::string conflict = keys_manifest_conflict(folder);
    if (!conflict.empty()) {
        refused.problem.lines.push_back(conflict);
        return report(folder, std::nullopt, refused, as_json, nullptr, output, strict, shown);
    }
    const KeyFolder inspected = inspect_key_folder(folder);
    if (!inspected.have_files || !inspected.problems.empty()) {
        refused.problem.lines = inspected.problems;
        if (refused.problem.lines.empty())
            refused.problem.lines.push_back("oakeys.yaml: it cannot be read");
        return report(folder, std::nullopt, refused, as_json, nullptr, output, strict, shown);
    }

    std::string pack_problem;
    const std::optional<std::vector<uint8_t>> bytes =
        oa::data::key_library::package_bytes(inspected.files, &pack_problem);
    if (!bytes) {
        refused.problem.lines.push_back(
            pack_problem.empty() ? std::string{"the key layout cannot be packed"} : pack_problem
        );
        return report(folder, std::nullopt, refused, as_json, nullptr, output, strict, shown);
    }

    const fs::path temporary = temporary_oakeys();
    RemoveFile cleanup(temporary);
    std::string write_problem;
    if (!oa::data::key_library::write_package(temporary, *bytes, &write_problem)) {
        refused.problem.lines.push_back(
            write_problem.empty() ? std::string{"the key layout cannot be packed"} : write_problem
        );
        return report(folder, std::nullopt, refused, as_json, nullptr, output, strict, shown);
    }
    FileHash hashed;
    hashed.size = static_cast<uint64_t>(bytes->size());
    hashed.digest = sha256::digest_of(*bytes);
    const install::PackageResult opened = install::open_package(temporary);
    return report(temporary, hashed, opened, as_json, nullptr, output, strict, shown);
}

} // namespace

void describe_oamod(const install::Package& package, json::JsonWriter& writer) {
    const std::optional<mod_profile::Resolution> resolved = catalogue_resolution(package);
    if (!resolved)
        return;
    const mod_profile::ModProfile& profile = resolved->profile;
    const mod_profile::Value& effective = resolved->effective;
    write_text(writer, "summary", profile.description);
    write_text(writer, "homepage", profile.homepage);
    if (!profile.tags.empty()) {
        writer.key("tags");
        write_strings(writer, profile.tags);
    }
    write_game_lists(writer, profile.supported_games, profile.required_expansions);
    write_text(writer, "author", profile.author.name);
    write_requires(effective, profile.requires_engine, writer);
    write_text(writer, "sim_hash", hex_text(profile.sim_hash));
    write_text(writer, "full_hash", hex_text(profile.full_hash));

    const std::vector<HackOn> on = hacks_on(effective);
    int64_t sim = 0;
    int64_t view = 0;
    std::vector<std::string> ids;
    std::vector<std::string> unimplemented;
    ids.reserve(on.size());
    for (const HackOn& hack : on) {
        if (hack.scope == mod_profile::registry::Scope::sim)
            ++sim;
        else if (hack.scope == mod_profile::registry::Scope::view)
            ++view;
        ids.push_back(hack.id);
        if (!hack.implemented)
            unimplemented.push_back(hack.id);
    }
    writer.key("hacks");
    writer.begin_object();
    writer.key("sim");
    writer.integer(sim);
    writer.key("view");
    writer.integer(view);
    writer.key("ids");
    write_strings(writer, ids);
    writer.key("unimplemented");
    write_strings(writer, unimplemented);
    writer.end_object();
    write_packaging(effective, writer);
}

void describe_oalang(std::span<const uint8_t> manifest, json::JsonWriter& writer) {
    const LanguageFields fields = read_language(manifest);
    if (!fields.read)
        return;
    write_oalang_identity(fields, writer);
    if (fields.games_known)
        write_game_lists(writer, fields.supported_games, fields.required_expansions);
    write_oalang_language(fields, writer);
}

int run_check(std::span<const std::string> arguments, Output& output) {
    static constexpr OptionSpec options[] = {
        {"game-dir", true, false},
        {"mod", true, true},
        {"accept-unimplemented-hacks", false, false},
        {"json", false, false},
        {"strict", false, false},
    };
    std::string problem;
    const std::optional<Arguments> parsed = parse_arguments(arguments, options, problem);
    if (!parsed) {
        report_usage(output, problem);
        return exit_usage;
    }
    if (parsed->positional.size() != 1) {
        report_usage(
            output, "expected one package file, got " + std::to_string(parsed->positional.size())
        );
        return exit_usage;
    }
    const std::string* const game_dir = parsed->value("game-dir");
    if (game_dir != nullptr && game_dir->empty()) {
        report_usage(output, "option '--game-dir' needs a value");
        return exit_usage;
    }
    const auto mods = parsed->values.find("mod");
    if (mods != parsed->values.end()) {
        for (const std::string& text : mods->second) {
            if (!mod_value_ok(text)) {
                report_usage(
                    output, "option '--mod' needs a folder, and a key after '=' when '=' is written"
                );
                return exit_usage;
            }
        }
    }

    const fs::path file = path_of(parsed->positional[0]);
    const bool strict = parsed->flags.contains("strict");
    std::error_code directory_error;
    if (fs::is_directory(fs::symlink_status(file, directory_error)) && folder_holds_oakeys(file)) {
        if (game_dir != nullptr) {
            report_usage(output, "option '--game-dir' is not for a key layout");
            return exit_usage;
        }
        return check_key_folder(file, parsed->flags.contains("json"), strict, output);
    }
    const std::optional<FileHash> hashed = hash_file(file);
    install::PackageOptions package_options{};
    package_options.accept_unimplemented_hacks =
        parsed->flags.contains("accept-unimplemented-hacks");
    if (game_dir != nullptr)
        package_options.game_folder = path_of(*game_dir);
    const install::PackageResult opened = install::open_package(file, package_options);

    const install::Package* package = opened.package ? &*opened.package : nullptr;
    const install::PackageKind* kind = install::kind_for_file(file);
    if (package != nullptr && package->kind != nullptr)
        kind = package->kind;
    if (kind != nullptr && kind->name == "expansion" && game_dir != nullptr) {
        report_usage(output, "option '--game-dir' is not for an expansion");
        return exit_usage;
    }
    if (kind != nullptr && kind->name == "oabrain" && game_dir != nullptr) {
        report_usage(output, "option '--game-dir' is not for a brain");
        return exit_usage;
    }
    if (kind != nullptr && kind->name == "oakeys" && game_dir != nullptr) {
        report_usage(output, "option '--game-dir' is not for a key layout");
        return exit_usage;
    }
    const bool accepted_map = package != nullptr && kind != nullptr && kind->name == "oamap";
    if (accepted_map && game_dir == nullptr) {
        report_usage(output, "option '--game-dir' is required for a map pack");
        return exit_usage;
    }
    std::optional<MapCheckRequest> map_request;
    if (accepted_map) {
        MapCheckRequest request;
        request.pack = file;
        request.game_dir = path_of(*game_dir);
        if (mods != parsed->values.end()) {
            request.mods.reserve(mods->second.size());
            for (const std::string& text : mods->second)
                request.mods.push_back(mod_target(text));
        }
        map_request = std::move(request);
    }
    return report(
        file,
        hashed,
        opened,
        parsed->flags.contains("json"),
        map_request ? &*map_request : nullptr,
        output,
        strict,
        {}
    );
}

} // namespace oa::tool
