// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The catalogue cache under <data>/content/catalogues/<registry id>/.
#include "service_state.hpp"

#include "oa/data/catalogue/releases.hpp"
#include "oa/formats/oamod.hpp"
#include "oa/platform/files.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace oa::app::content {
namespace {

namespace fs = std::filesystem;

/// How a cache file was read.
enum class ReadKind : uint8_t { missing, ok, failed };

/// The bytes of one cache file, or why they were not read.
struct ReadFile {
    ReadKind kind = ReadKind::missing;
    std::vector<uint8_t> bytes{};
    std::string error{};
};

/// The fields state.yaml held. Each is empty when the file did not name it.
struct CacheNote {
    std::optional<int64_t> sequence{};
    std::optional<int64_t> checked{};
    std::optional<formats::url::Url> source{};
};

/// Reports whether a registry id can be one path segment inside the cache.
///
/// @param id the registry id
/// @return true when the id has no separator and is not a relative segment
bool safe_registry_id(std::string_view id) {
    if (id.empty() || id == "." || id == "..")
        return false;
    for (char character : id) {
        if (character == '/' || character == '\\' || character == ':')
            return false;
    }
    return true;
}

/// Reads a whole file that is no larger than `limit`.
///
/// @param path the file
/// @param limit the most bytes that are read
/// @return the bytes, or missing, or failed
ReadFile read_limited(const fs::path& path, uint64_t limit) {
    ReadFile read;
    std::error_code error;
    const fs::file_status status = fs::status(path, error);
    if (error || status.type() == fs::file_type::not_found || status.type() == fs::file_type::none)
        return read;
    if (!fs::is_regular_file(status)) {
        read.kind = ReadKind::failed;
        read.error = "not a file";
        return read;
    }
    const auto size = fs::file_size(path, error);
    if (error) {
        read.kind = ReadKind::failed;
        read.error = error.message();
        return read;
    }
    if (size > limit) {
        read.kind = ReadKind::failed;
        read.error = "larger than the limit";
        return read;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        read.kind = ReadKind::failed;
        read.error = "could not be opened";
        return read;
    }
    read.bytes.resize(static_cast<std::size_t>(size));
    if (size > 0 &&
        !in.read(reinterpret_cast<char*>(read.bytes.data()), static_cast<std::streamsize>(size))) {
        read.kind = ReadKind::failed;
        read.error = "could not be read";
        read.bytes.clear();
        return read;
    }
    read.kind = ReadKind::ok;
    return read;
}

/// Reads a whole decimal integer from a string.
///
/// @param text the digits, with an optional leading minus
/// @return the value, or nothing when the text is not one integer
std::optional<int64_t> parse_decimal(std::string_view text) {
    if (text.empty())
        return std::nullopt;
    std::size_t index = 0;
    bool negative = false;
    if (text[0] == '-') {
        negative = true;
        index = 1;
        if (index >= text.size())
            return std::nullopt;
    }
    int64_t value = 0;
    for (; index < text.size(); ++index) {
        const char character = text[index];
        if (character < '0' || character > '9')
            return std::nullopt;
        const int digit = character - '0';
        if (value > (INT64_MAX - digit) / 10)
            return std::nullopt;
        value = value * 10 + digit;
    }
    if (negative)
        return -value;
    return value;
}

/// Reads an integer node, whether it was quoted.
///
/// @param node the node
/// @return the integer, or nothing when the node is not one
std::optional<int64_t> integer_of(const formats::oamod::Node& node) {
    if (node.kind == formats::oamod::NodeKind::number) {
        int64_t value = 0;
        if (!formats::oamod::integer_value(node.number, value))
            return std::nullopt;
        return value;
    }
    if (node.kind == formats::oamod::NodeKind::string)
        return parse_decimal(node.text);
    return std::nullopt;
}

/// Reads state.yaml. A file that cannot be read yields empty fields.
///
/// @param bytes the file's bytes
/// @return the fields that were present
CacheNote read_note(std::span<const uint8_t> bytes) {
    CacheNote note;
    formats::oamod::Node root;
    formats::oamod::ReadError error;
    if (!formats::oamod::read_document(bytes, root, error))
        return note;
    if (const formats::oamod::Node* sequence = formats::oamod::find_entry(root, "sequence"))
        note.sequence = integer_of(*sequence);
    if (const formats::oamod::Node* checked = formats::oamod::find_entry(root, "checked"))
        note.checked = integer_of(*checked);
    if (const formats::oamod::Node* source = formats::oamod::find_entry(root, "source")) {
        if (source->kind == formats::oamod::NodeKind::string)
            note.source = formats::url::parse_http_url(source->text);
    }
    return note;
}

/// Quotes a string for state.yaml, escaping backslash and quote.
///
/// @param text the string
/// @return a double-quoted scalar
std::string yaml_quote(std::string_view text) {
    std::string quoted;
    quoted.push_back('"');
    for (char character : text) {
        if (character == '\\' || character == '"')
            quoted.push_back('\\');
        quoted.push_back(character);
    }
    quoted.push_back('"');
    return quoted;
}

/// The state.yaml text for one accepted catalogue.
///
/// @param sequence the catalogue's sequence
/// @param digest the SHA-256 of the catalogue bytes
/// @param checked when the catalogue was confirmed, seconds since 1970
/// @param source the URL that served it
/// @return the file's text
std::string note_text(
    int64_t sequence,
    const base::sha256::Digest& digest,
    int64_t checked,
    const formats::url::Url& source
) {
    const auto hex = base::sha256::to_hex(digest);
    std::string text = "sequence: ";
    text += std::to_string(sequence);
    text += "\nsha256: ";
    text += yaml_quote(std::string_view(hex.data(), hex.size()));
    text += "\nchecked: ";
    text += std::to_string(checked);
    text += "\nsource: ";
    text += yaml_quote(formats::url::url_text(source));
    text += '\n';
    return text;
}

/// The cache directory of one registry, or empty when it cannot be stored.
///
/// @param state the service
/// @param id the registry id
/// @return the directory, or empty
fs::path cache_directory(const ServiceState& state, std::string_view id) {
    if (state.options.data_folder.empty() || !safe_registry_id(id))
        return {};
    return state.options.data_folder / std::string(content_folder_name) /
           std::string(catalogues_folder_name) / std::string(id);
}

/// Replaces one file and records a failure.
///
/// @param file the file
/// @param bytes the bytes
/// @param error set when the file was not replaced
/// @return true when the file holds these bytes
bool replace_bytes(const fs::path& file, std::span<const uint8_t> bytes, std::string& error) {
    return oa::platform::replace_file(file, bytes, &error);
}

} // namespace

void read_cache(ServiceState& state, RegistryRecord& record) {
    const std::string& id = record.registry.descriptor.id;
    const fs::path directory = cache_directory(state, id);
    if (directory.empty())
        return;
    const ReadFile catalogue_file =
        read_limited(directory / "catalogue.json", data::catalogue::max_catalogue_bytes);
    if (catalogue_file.kind == ReadKind::missing)
        return;
    if (catalogue_file.kind == ReadKind::failed) {
        log_line(state, "cache of " + id + ": the cached catalogue could not be read");
        return;
    }

    std::vector<uint8_t> signature;
    const ReadFile signature_file = read_limited(directory / "catalogue.json.sig", 4 * 1024);
    if (signature_file.kind == ReadKind::failed) {
        if (record.registry.descriptor.is_signed()) {
            log_line(state, "cache of " + id + ": the cached signature could not be read");
            return;
        }
    } else if (signature_file.kind == ReadKind::ok) {
        signature = signature_file.bytes;
    }

    const int64_t now = now_seconds(state);
    data::catalogue::CheckRequest request;
    request.catalogue = catalogue_file.bytes;
    request.signature = signature;
    request.registry_id = id;
    request.trusted_keys = record.registry.descriptor.keys;
    request.built_in = record.registry.origin == data::registry::Origin::built_in;
    request.now = now;
    const data::catalogue::Checked checked = data::catalogue::check_catalogue(request);
    if (!checked.usable() || !checked.catalogue) {
        log_line(
            state, "cache of " + id + ": refused, " + data::catalogue::verdict_text(checked.verdict)
        );
        return;
    }

    const ReadFile note_file = read_limited(directory / "state.yaml", 64 * 1024);
    CacheNote note;
    if (note_file.kind == ReadKind::ok)
        note = read_note(note_file.bytes);

    record.bytes = catalogue_file.bytes;
    record.signature = std::move(signature);
    record.catalogue = checked.catalogue;
    record.digest = checked.digest;
    record.have_digest = true;
    record.expired = checked.expired;
    record.checked = note.checked;
    record.served_from = note.source;
    record.refresh_failed = false;
    record.sources = sources_of(record);
    record.releases.reset();
    record.releases_digest.reset();
    if (!record.catalogue->releases.has_value())
        return;
    const ReadFile releases_file =
        read_limited(directory / "releases.json", data::catalogue::max_catalogue_bytes);
    if (releases_file.kind != ReadKind::ok)
        return;
    data::catalogue::ReleasesTake taken = data::catalogue::ReleasesTake::mismatch;
    data::catalogue::HeldReleases held = data::catalogue::take_releases(
        *record.catalogue->releases, releases_file.bytes, {}, &taken
    );
    if (taken != data::catalogue::ReleasesTake::taken) {
        log_line(state, "cache of " + id + ": the cached releases list was not kept");
        return;
    }
    record.releases_digest = held.digest;
    record.releases = std::make_shared<const data::catalogue::ReleasesList>(std::move(held.list));
}

void write_cache(
    const ServiceState& state,
    std::string_view id,
    const data::catalogue::Catalogue& catalogue,
    const base::sha256::Digest& digest,
    int64_t checked,
    const formats::url::Url& source,
    std::span<const uint8_t> catalogue_bytes,
    std::span<const uint8_t> signature,
    bool write_catalogue
) {
    const fs::path directory = cache_directory(state, id);
    if (directory.empty()) {
        if (!state.options.data_folder.empty())
            log_line(state, "cache of " + std::string(id) + ": the catalogue was not saved");
        return;
    }
    if (write_catalogue) {
        std::string error;
        if (!replace_bytes(directory / "catalogue.json", catalogue_bytes, error)) {
            log_line(
                state, "cache of " + std::string(id) + ": the catalogue was not saved: " + error
            );
            return;
        }
        if (!signature.empty() &&
            !replace_bytes(directory / "catalogue.json.sig", signature, error)) {
            log_line(
                state, "cache of " + std::string(id) + ": the signature was not saved: " + error
            );
            return;
        }
    }
    const std::string text = note_text(catalogue.sequence, digest, checked, source);
    std::string error;
    const auto* const bytes = reinterpret_cast<const uint8_t*>(text.data());
    if (!replace_bytes(
            directory / "state.yaml", std::span<const uint8_t>(bytes, text.size()), error
        ))
        log_line(
            state, "cache of " + std::string(id) + ": the catalogue state was not saved: " + error
        );
}

void write_releases_cache(
    const ServiceState& state, std::string_view id, std::span<const uint8_t> bytes
) {
    const fs::path directory = cache_directory(state, id);
    if (directory.empty())
        return;
    std::string error;
    if (!replace_bytes(directory / "releases.json", bytes, error))
        log_line(
            state, "cache of " + std::string(id) + ": the releases list was not saved: " + error
        );
}

} // namespace oa::app::content
