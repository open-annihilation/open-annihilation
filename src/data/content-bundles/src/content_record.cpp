// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Reading and writing one bundle folder's .oa-content record (record.hpp).
#include "oa/data/content_bundles/record.hpp"

#include <charconv>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace oa::data::content_bundles {
namespace {

constexpr uint64_t kRecordByteLimit = uint64_t{1024} * 1024;

void append_u64(std::string& text, uint64_t value) {
    char buffer[20];
    const auto converted = std::to_chars(buffer, buffer + sizeof buffer, value);
    text.append(buffer, static_cast<std::size_t>(converted.ptr - buffer));
}

void append_i64(std::string& text, int64_t value) {
    if (value < 0) {
        text.push_back('-');
        const uint64_t magnitude =
            value == std::numeric_limits<int64_t>::min()
                ? static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1
                : static_cast<uint64_t>(-value);
        append_u64(text, magnitude);
        return;
    }
    append_u64(text, static_cast<uint64_t>(value));
}

[[nodiscard]] std::string escape_field(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (const char byte : text) {
        if (byte == '\\')
            escaped += "\\\\";
        else if (byte == '\n')
            escaped += "\\n";
        else if (byte == '\r')
            escaped += "\\r";
        else
            escaped.push_back(byte);
    }
    return escaped;
}

[[nodiscard]] bool unescape_field(std::string_view text, std::string& decoded) {
    decoded.clear();
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] != '\\') {
            decoded.push_back(text[index]);
            continue;
        }
        if (index + 1 >= text.size())
            return false;
        const char next = text[++index];
        if (next == '\\')
            decoded.push_back('\\');
        else if (next == 'n')
            decoded.push_back('\n');
        else if (next == 'r')
            decoded.push_back('\r');
        else
            return false;
    }
    return true;
}

[[nodiscard]] bool parse_u64(std::string_view text, uint64_t& value) noexcept {
    if (text.empty())
        return false;
    uint64_t parsed = 0;
    for (const char byte : text) {
        if (byte < '0' || byte > '9')
            return false;
        const uint64_t digit = static_cast<uint64_t>(byte - '0');
        if (parsed > (std::numeric_limits<uint64_t>::max() - digit) / 10)
            return false;
        parsed = parsed * 10 + digit;
    }
    value = parsed;
    return true;
}

[[nodiscard]] bool parse_i64(std::string_view text, int64_t& value) noexcept {
    if (text.empty())
        return false;
    bool negative = false;
    if (text.front() == '-') {
        negative = true;
        text.remove_prefix(1);
    }
    uint64_t magnitude = 0;
    if (!parse_u64(text, magnitude))
        return false;
    if (!negative) {
        if (magnitude > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
            return false;
        value = static_cast<int64_t>(magnitude);
        return true;
    }
    const uint64_t limit = static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1;
    if (magnitude > limit)
        return false;
    if (magnitude == limit)
        value = std::numeric_limits<int64_t>::min();
    else
        value = -static_cast<int64_t>(magnitude);
    return true;
}

[[nodiscard]] bool
take_prefix(std::string_view line, std::string_view prefix, std::string_view& rest) {
    if (line.size() < prefix.size() || line.substr(0, prefix.size()) != prefix)
        return false;
    rest = line.substr(prefix.size());
    return true;
}

[[nodiscard]] std::string_view field_name(std::string_view line) {
    const std::size_t space = line.find(' ');
    if (space == std::string_view::npos)
        return line;
    return line.substr(0, space);
}

[[nodiscard]] bool known_field(std::string_view name) {
    return name == "bundle" || name == "source" || name == "file" || name == "size" ||
           name == "modified" || name == "sha256";
}

[[nodiscard]] bool one_line_name(std::string_view name) {
    return name.find('\n') == std::string_view::npos && name.find('\r') == std::string_view::npos;
}

/// Reads a package line's fields: a SHA-256, one space and a release.
///
/// @param text what follows "package "
/// @return the package, or nothing when the fields are not those
[[nodiscard]] std::optional<ContentRecordPackage> parse_package(std::string_view text) {
    const std::size_t space = text.find(' ');
    if (space == std::string_view::npos)
        return std::nullopt;
    const auto digest = base::sha256::parse_hex(text.substr(0, space));
    if (!digest)
        return std::nullopt;
    uint64_t release = 0;
    if (!parse_u64(text.substr(space + 1), release) ||
        release > static_cast<uint64_t>(content_record_release_limit))
        return std::nullopt;
    ContentRecordPackage package;
    package.sha256 = *digest;
    package.release = static_cast<int64_t>(release);
    return package;
}

void append_line(std::string& text, std::string_view line) {
    text.append(line);
    text.push_back('\n');
}

[[nodiscard]] std::vector<std::string_view> lines_of(std::string_view text) {
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    for (std::size_t index = 0; index <= text.size(); ++index) {
        if (index != text.size() && text[index] != '\n')
            continue;
        std::string_view line = text.substr(start, index - start);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        lines.push_back(line);
        start = index + 1;
    }
    if (!lines.empty() && lines.back().empty())
        lines.pop_back();
    return lines;
}

void set_error(std::string* error, std::string_view text) {
    if (error != nullptr)
        *error = std::string(text);
}

[[nodiscard]] char fold_ascii(char byte) noexcept {
    if (byte >= 'A' && byte <= 'Z')
        return static_cast<char>(byte - 'A' + 'a');
    return byte;
}

[[nodiscard]] bool same_name(std::string_view one, std::string_view other) noexcept {
    if (one.size() != other.size())
        return false;
    for (std::size_t index = 0; index < one.size(); ++index) {
        if (fold_ascii(one[index]) != fold_ascii(other[index]))
            return false;
    }
    return true;
}

} // namespace

ContentRecord parse_content_record(std::string_view text) {
    ContentRecord record;
    const std::vector<std::string_view> lines = lines_of(text);
    if (lines.empty() || lines[0] != "oa-content 1")
        return record;
    bool saw_bundle = false;
    bool saw_source = false;
    std::optional<ContentRecordPackage> package;
    std::vector<ContentRecordFile> files;
    for (std::size_t index = 1; index < lines.size();) {
        const std::string_view line = lines[index];
        const std::string_view name = field_name(line);
        if (name.empty())
            return record;
        // Written after a file's sha256. Older readers skip the line and
        // still read the file, size, time and hash.
        if (line == "not-in-source") {
            if (!files.empty())
                files.back().not_in_source = true;
            ++index;
            continue;
        }
        // Written after the source, for an expansion installed from a
        // package. Older readers skip it as a field they do not know, and a
        // line this reader cannot use is skipped the same way.
        std::string_view package_text;
        if (take_prefix(line, "package ", package_text)) {
            if (saw_source && !package)
                package = parse_package(package_text);
            ++index;
            continue;
        }
        // Written after "not-in-source", for a file the source held at
        // another size. Older readers skip it as a field they do not know.
        std::string_view source_size_text;
        if (take_prefix(line, "source-size ", source_size_text)) {
            uint64_t source_size = 0;
            if (!parse_u64(source_size_text, source_size))
                return record;
            if (!files.empty() && files.back().not_in_source)
                files.back().source_size = source_size;
            ++index;
            continue;
        }
        if (!known_field(name)) {
            ++index;
            continue;
        }
        std::string_view bundle_id;
        if (take_prefix(line, "bundle ", bundle_id)) {
            if (saw_bundle || saw_source || bundle_id.empty() || !one_line_name(bundle_id))
                return record;
            record.bundle_id = std::string(bundle_id);
            saw_bundle = true;
            ++index;
            continue;
        }
        std::string_view source_field;
        if (take_prefix(line, "source ", source_field)) {
            if (!saw_bundle || saw_source)
                return record;
            if (!unescape_field(source_field, record.source))
                return record;
            saw_source = true;
            ++index;
            continue;
        }
        std::string_view copy_name;
        if (!take_prefix(line, "file ", copy_name) || !saw_source || copy_name.empty() ||
            !one_line_name(copy_name) || index + 3 >= lines.size())
            return record;
        std::string_view size_text;
        std::string_view modified_text;
        std::string_view hash_text;
        if (!take_prefix(lines[index + 1], "size ", size_text) ||
            !take_prefix(lines[index + 2], "modified ", modified_text) ||
            !take_prefix(lines[index + 3], "sha256 ", hash_text))
            return record;
        ContentRecordFile item;
        item.copy_name = std::string(copy_name);
        if (!parse_u64(size_text, item.size) || !parse_i64(modified_text, item.modified))
            return record;
        const auto digest = base::sha256::parse_hex(hash_text);
        if (!digest)
            return record;
        item.digest = *digest;
        files.push_back(std::move(item));
        index += 4;
    }
    if (!saw_bundle || !saw_source)
        return record;
    record.valid = true;
    record.package = package;
    record.files = std::move(files);
    return record;
}

std::optional<std::string> format_content_record(const ContentRecord& record) {
    if (!one_line_name(record.bundle_id) || record.bundle_id.empty())
        return std::nullopt;
    for (const ContentRecordFile& item : record.files)
        if (!one_line_name(item.copy_name) || item.copy_name.empty())
            return std::nullopt;
    std::string text;
    append_line(text, "oa-content 1");
    append_line(text, "bundle " + record.bundle_id);
    append_line(text, "source " + escape_field(record.source));
    if (record.package) {
        if (record.package->release < 0 || record.package->release > content_record_release_limit)
            return std::nullopt;
        const auto package_hex = base::sha256::to_hex(record.package->sha256);
        std::string package_line = "package ";
        package_line.append(package_hex.data(), package_hex.size());
        package_line.push_back(' ');
        append_u64(package_line, static_cast<uint64_t>(record.package->release));
        append_line(text, package_line);
    }
    for (const ContentRecordFile& item : record.files) {
        append_line(text, "file " + item.copy_name);
        std::string size_line = "size ";
        append_u64(size_line, item.size);
        append_line(text, size_line);
        std::string time_line = "modified ";
        append_i64(time_line, item.modified);
        append_line(text, time_line);
        const auto hex = base::sha256::to_hex(item.digest);
        std::string hash_line = "sha256 ";
        hash_line.append(hex.data(), hex.size());
        append_line(text, hash_line);
        if (item.not_in_source)
            append_line(text, "not-in-source");
        if (item.not_in_source && item.source_size > 0) {
            std::string source_size_line = "source-size ";
            append_u64(source_size_line, item.source_size);
            append_line(text, source_size_line);
        }
    }
    return text;
}

ContentRecord read_bundle_folder_record(const std::filesystem::path& folder, std::string* error) {
    ContentRecord record;
    if (error != nullptr)
        error->clear();
    const std::filesystem::path path = folder / std::string(content_record_name);
    std::error_code failure;
    const std::filesystem::file_status status = std::filesystem::status(path, failure);
    // A path that is not there, including a bundle folder that has not been
    // created yet, is a missing record. status reports that as an error.
    if (failure == std::errc::no_such_file_or_directory || failure == std::errc::not_a_directory ||
        (!failure && !std::filesystem::exists(status)))
        return record;
    if (failure) {
        record.present = true;
        record.io_error = true;
        set_error(error, "the record could not be read");
        return record;
    }
    record.present = true;
    if (!std::filesystem::is_regular_file(status))
        return record;
    std::ifstream input(path, std::ios::in | std::ios::binary);
    if (!input) {
        record.io_error = true;
        set_error(error, "the record could not be read");
        return record;
    }
    std::string text;
    char buffer[4096];
    while (text.size() <= kRecordByteLimit) {
        input.read(buffer, static_cast<std::streamsize>(sizeof buffer));
        const auto read = input.gcount();
        if (read > 0)
            text.append(buffer, static_cast<std::size_t>(read));
        if (read < static_cast<std::streamsize>(sizeof buffer))
            break;
    }
    if (input.bad()) {
        record.io_error = true;
        set_error(error, "the record could not be read");
        return record;
    }
    if (text.size() > kRecordByteLimit) {
        record.io_error = true;
        set_error(error, "the record could not be read");
        return record;
    }
    ContentRecord parsed = parse_content_record(text);
    parsed.present = true;
    return parsed;
}

std::vector<std::string_view>
files_not_recorded(const Bundle& bundle, const ContentRecord& record) {
    std::vector<std::string_view> missing;
    for (const ContentFile& file : bundle.files) {
        bool named = false;
        for (const ContentRecordFile& item : record.files) {
            if (same_name(item.copy_name, file.copy_name)) {
                named = true;
                break;
            }
        }
        if (!named)
            missing.push_back(file.copy_name);
    }
    return missing;
}

bool record_complete(const Bundle& bundle, const ContentRecord& record) {
    if (!record.valid || record.bundle_id != bundle.id || bundle.files.empty())
        return false;
    return files_not_recorded(bundle, record).empty();
}

PackageCheck check_installed_package(
    const Bundle& bundle, const ContentRecord& record, const base::sha256::Digest& offered
) {
    if (!record_complete(bundle, record))
        return PackageCheck::missing;
    if (!record.package)
        return PackageCheck::unrecorded;
    if (record.package->sha256 != offered)
        return PackageCheck::changed;
    return PackageCheck::current;
}

} // namespace oa::data::content_bundles
