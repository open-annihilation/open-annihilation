// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/data/engine_feed/feed.hpp"

#include "oa/base/sha256.hpp"
#include "oa/data/catalogue/catalogue.hpp"
#include "oa/formats/json.hpp"
#include "oa/formats/oamod/package_keys.hpp"
#include "oa/formats/url.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::data::engine_feed {
namespace {

namespace json = oa::formats::json;
namespace oamod = oa::formats::oamod;
namespace url = oa::formats::url;
namespace catalogue = oa::data::catalogue;

/// The most releases one feed lists.
constexpr std::size_t max_releases = 32;

/// The greatest release number, 2^53.
constexpr int64_t max_release_number = static_cast<int64_t>(uint64_t{1} << 53);

/// The most bytes of one package file, 2^40.
constexpr int64_t max_file_bytes = static_cast<int64_t>(uint64_t{1} << 40);

/// The most note lines one language may carry.
constexpr std::size_t max_note_lines = 12;

/// The most bytes of one note line.
constexpr std::size_t max_note_line_bytes = 200;

/// The most bytes of a plain file name.
constexpr std::size_t max_file_name_bytes = 128;

/// Records `message` when the caller asked for a reason.
void set_error(std::string* reason, std::string message) {
    if (reason != nullptr)
        *reason = std::move(message);
}

/// The feed's bytes as text. An empty span is an empty view.
std::string_view bytes_as_text(std::span<const uint8_t> bytes) {
    if (bytes.empty())
        return {};
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

/// Reports whether every object names each of its members once.
///
/// A member named `packages` whose value is an object uses the package
/// wording for a repeated name. Every other object uses the general wording.
bool names_unique(const json::Json& value, std::string* reason, bool packages_object) {
    if (value.type() == json::JsonType::array) {
        for (const json::Json& element : value.elements()) {
            if (!names_unique(element, reason, false))
                return false;
        }
        return true;
    }
    if (value.type() != json::JsonType::object)
        return true;
    const auto member_names = value.names();
    for (std::size_t index = 0; index < member_names.size(); ++index) {
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            if (member_names[earlier] == member_names[index]) {
                set_error(
                    reason, packages_object ? "a package is named twice" : "a member is named twice"
                );
                return false;
            }
        }
    }
    const auto member_values = value.values();
    for (std::size_t index = 0; index < member_names.size(); ++index) {
        const bool child_is_packages = !packages_object && member_names[index] == "packages" &&
                                       member_values[index].type() == json::JsonType::object;
        if (!names_unique(member_values[index], reason, child_is_packages))
            return false;
    }
    return true;
}

/// True when `text` is one or more lower-case letters and digits.
bool channel_name(std::string_view text) {
    if (text.empty())
        return false;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const unsigned char byte = static_cast<unsigned char>(text[index]);
        const bool letter = byte >= 'a' && byte <= 'z';
        const bool digit = byte >= '0' && byte <= '9';
        if (!letter && !digit)
            return false;
    }
    return true;
}

/// True when `text` matches `[a-z0-9]+(-[a-z0-9_]+)*`.
bool package_id_text(std::string_view text) {
    std::size_t index = 0;
    auto piece = [&](bool allow_underscore) {
        const std::size_t start = index;
        while (index < text.size()) {
            const unsigned char byte = static_cast<unsigned char>(text[index]);
            const bool letter = byte >= 'a' && byte <= 'z';
            const bool digit = byte >= '0' && byte <= '9';
            const bool underscore = allow_underscore && byte == '_';
            if (!letter && !digit && !underscore)
                break;
            ++index;
        }
        return index > start;
    };
    if (!piece(false))
        return false;
    while (index < text.size()) {
        if (text[index] != '-')
            return false;
        ++index;
        if (!piece(true))
            return false;
    }
    return true;
}

/// True when `text` matches `[a-z]{2,3}(-[A-Za-z0-9]{2,8})*`.
bool language_tag(std::string_view text) {
    std::size_t index = 0;
    std::size_t primary = 0;
    while (index < text.size()) {
        const unsigned char byte = static_cast<unsigned char>(text[index]);
        if (byte < 'a' || byte > 'z')
            break;
        ++primary;
        ++index;
    }
    if (primary < 2 || primary > 3)
        return false;
    while (index < text.size()) {
        if (text[index] != '-')
            return false;
        ++index;
        std::size_t sub = 0;
        while (index < text.size()) {
            const unsigned char byte = static_cast<unsigned char>(text[index]);
            const bool letter = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z');
            const bool digit = byte >= '0' && byte <= '9';
            if (!letter && !digit)
                break;
            ++sub;
            ++index;
        }
        if (sub < 2 || sub > 8)
            return false;
    }
    return index == text.size();
}

/// True when `text` is a plain file name: 1 to 128 of `[A-Za-z0-9._-]`, not `.` or `..`.
bool plain_file_name(std::string_view text) {
    if (text.empty() || text.size() > max_file_name_bytes || text == "." || text == "..")
        return false;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const unsigned char byte = static_cast<unsigned char>(text[index]);
        const bool letter = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z');
        const bool digit = byte >= '0' && byte <= '9';
        const bool mark = byte == '.' || byte == '_' || byte == '-';
        if (!letter && !digit && !mark)
            return false;
    }
    return true;
}

/// True when `text` is exactly 64 digits of `0-9` and `a-f`.
bool lower_hex(std::string_view text) {
    if (text.size() != base::sha256::hex_size)
        return false;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const unsigned char byte = static_cast<unsigned char>(text[index]);
        const bool digit = byte >= '0' && byte <= '9';
        const bool letter = byte >= 'a' && byte <= 'f';
        if (!digit && !letter)
            return false;
    }
    return true;
}

/// True when `text` holds a C0 control, DEL, or a C1 control encoded in UTF-8.
bool control_character(std::string_view text) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
    for (std::size_t index = 0; index < text.size(); ++index) {
        const unsigned char byte = bytes[index];
        if (byte < 0x20 || byte == 0x7F)
            return true;
        if (byte == 0xC2 && index + 1 < text.size()) {
            const unsigned char next = bytes[index + 1];
            if (next >= 0x80 && next <= 0x9F)
                return true;
        }
    }
    return false;
}

/// An http or https address whose path ends in `/` and which has no query.
std::optional<url::Url> read_mirror(std::string_view text) {
    const std::optional<url::Url> parsed = url::parse_http_url(text);
    if (!parsed || url::has_query(*parsed))
        return std::nullopt;
    if (parsed->target.empty() || parsed->target.back() != '/')
        return std::nullopt;
    return parsed;
}

/// Reads file, size and sha256 from an object that has those members.
bool read_package_file(const json::Json& object, PackageFile& file, std::string* reason) {
    const json::Json* file_member = object.find("file");
    const std::string* file_text = file_member != nullptr ? file_member->string() : nullptr;
    if (file_text == nullptr) {
        set_error(reason, "file is missing");
        return false;
    }
    if (!plain_file_name(*file_text)) {
        set_error(reason, "file is not a plain name");
        return false;
    }
    const json::Json* size_member = object.find("size");
    const std::optional<int64_t> size =
        size_member != nullptr ? size_member->integer() : std::nullopt;
    if (!size || *size < 1 || *size > max_file_bytes) {
        set_error(reason, "size is not a whole number from 1 to 2^40");
        return false;
    }
    const json::Json* hash_member = object.find("sha256");
    const std::string* hash_text = hash_member != nullptr ? hash_member->string() : nullptr;
    if (hash_text == nullptr || !lower_hex(*hash_text)) {
        set_error(reason, "sha256 is not 64 lower-case hex digits");
        return false;
    }
    const std::optional<base::sha256::Digest> digest = base::sha256::parse_hex(*hash_text);
    if (!digest) {
        set_error(reason, "sha256 is not 64 lower-case hex digits");
        return false;
    }
    file.file = *file_text;
    file.size = static_cast<uint64_t>(*size);
    file.sha256 = *digest;
    return true;
}

/// Reads one package. `name` is its id.
bool read_package(
    std::string_view name, const json::Json& value, Package& package, std::string* reason
) {
    if (!package_id_text(name)) {
        set_error(reason, "a package id is not a package id");
        return false;
    }
    if (value.type() != json::JsonType::object) {
        set_error(reason, "a package is not an object");
        return false;
    }
    if (!read_package_file(value, package.zip, reason))
        return false;
    if (const json::Json* installer = value.find("installer")) {
        const bool shaped =
            installer->type() == json::JsonType::object && installer->find("file") != nullptr &&
            installer->find("size") != nullptr && installer->find("sha256") != nullptr;
        if (!shaped) {
            set_error(reason, "installer is not a file, size and sha256");
            return false;
        }
        PackageFile installer_file;
        if (!read_package_file(*installer, installer_file, reason))
            return false;
        package.installer = std::move(installer_file);
    }
    const json::Json* mirrors = value.find("mirrors");
    if (mirrors == nullptr || mirrors->type() != json::JsonType::array) {
        set_error(reason, "mirrors is not a list");
        return false;
    }
    if (mirrors->elements().empty()) {
        set_error(reason, "mirrors is empty");
        return false;
    }
    for (const json::Json& element : mirrors->elements()) {
        const std::string* text = element.string();
        const std::optional<url::Url> mirror = text != nullptr ? read_mirror(*text) : std::nullopt;
        if (!mirror) {
            set_error(reason, "a mirror is not an http or https address ending in / with no query");
            return false;
        }
        package.mirrors.push_back(*mirror);
    }
    package.id = std::string(name);
    return true;
}

/// Reads the notes object. The tag is checked before the lines, and `en` last.
bool read_notes(const json::Json& value, std::vector<Notes>& notes, std::string* reason) {
    if (value.type() != json::JsonType::object) {
        set_error(reason, "notes is not an object");
        return false;
    }
    bool has_english = false;
    const auto member_names = value.names();
    const auto member_values = value.values();
    for (std::size_t index = 0; index < member_names.size(); ++index) {
        const std::string& tag = member_names[index];
        if (!language_tag(tag)) {
            set_error(reason, "a notes language is not a language tag");
            return false;
        }
        if (tag == "en")
            has_english = true;
        const json::Json& lines = member_values[index];
        if (lines.type() != json::JsonType::array) {
            set_error(reason, "a notes language is not a list of lines");
            return false;
        }
        if (lines.elements().size() > max_note_lines) {
            set_error(reason, "a notes language has more than 12 lines");
            return false;
        }
        Notes one;
        one.language = tag;
        for (const json::Json& element : lines.elements()) {
            const std::string* text = element.string();
            if (text == nullptr) {
                set_error(reason, "a notes line is not text");
                return false;
            }
            if (text->size() > max_note_line_bytes) {
                set_error(reason, "a notes line is longer than 200 bytes");
                return false;
            }
            if (!json::utf8_valid(*text)) {
                set_error(reason, "a notes line is not valid UTF-8");
                return false;
            }
            if (control_character(*text)) {
                set_error(reason, "a notes line has a control character");
                return false;
            }
            one.lines.push_back(*text);
        }
        notes.push_back(std::move(one));
    }
    if (!has_english) {
        set_error(reason, "notes has no en");
        return false;
    }
    return true;
}

/// Reads one release object.
bool read_release(const json::Json& value, Release& release, std::string* reason) {
    if (value.type() != json::JsonType::object) {
        set_error(reason, "a release is not an object");
        return false;
    }
    const json::Json* number_member = value.find("release");
    if (number_member == nullptr) {
        set_error(reason, "release is missing");
        return false;
    }
    const std::optional<int64_t> number = number_member->integer();
    if (!number || *number < 1 || *number > max_release_number) {
        set_error(reason, "release is not a whole number from 1 to 2^53");
        return false;
    }
    const json::Json* version_member = value.find("version");
    const std::string* version_text =
        version_member != nullptr ? version_member->string() : nullptr;
    if (version_text == nullptr) {
        set_error(reason, "version is missing");
        return false;
    }
    const std::optional<oamod::EngineVersion> version = oamod::parse_engine_version(*version_text);
    if (!version) {
        set_error(reason, "version is not a version");
        return false;
    }
    const json::Json* date_member = value.find("date");
    const std::string* date_text = date_member != nullptr ? date_member->string() : nullptr;
    if (date_text == nullptr) {
        set_error(reason, "date is missing");
        return false;
    }
    if (!catalogue::parse_day(*date_text)) {
        set_error(reason, "date is not a day");
        return false;
    }
    const json::Json* from_member = value.find("from");
    const std::string* from_text = from_member != nullptr ? from_member->string() : nullptr;
    if (from_text == nullptr) {
        set_error(reason, "from is missing");
        return false;
    }
    const std::optional<oamod::EngineVersion> from = oamod::parse_engine_version(*from_text);
    if (!from) {
        set_error(reason, "from is not a version");
        return false;
    }
    int32_t rollout = 100;
    if (const json::Json* rollout_member = value.find("rollout")) {
        const std::optional<int64_t> rollout_number = rollout_member->integer();
        if (!rollout_number || *rollout_number < 0 || *rollout_number > 100) {
            set_error(reason, "rollout is not a whole number from 0 to 100");
            return false;
        }
        rollout = static_cast<int32_t>(*rollout_number);
    }
    if (const json::Json* notes_member = value.find("notes")) {
        if (!read_notes(*notes_member, release.notes, reason))
            return false;
    }
    std::string page;
    if (const json::Json* page_member = value.find("page")) {
        const std::string* page_text = page_member->string();
        if (page_text == nullptr || !url::parse_http_url(*page_text)) {
            set_error(reason, "page is not an http or https address");
            return false;
        }
        page = *page_text;
    }
    const json::Json* packages_member = value.find("packages");
    if (packages_member == nullptr) {
        set_error(reason, "packages is missing");
        return false;
    }
    if (packages_member->type() != json::JsonType::object) {
        set_error(reason, "packages is not an object");
        return false;
    }
    const auto package_names = packages_member->names();
    const auto package_values = packages_member->values();
    for (std::size_t index = 0; index < package_names.size(); ++index) {
        Package package;
        if (!read_package(package_names[index], package_values[index], package, reason))
            return false;
        release.packages.push_back(std::move(package));
    }
    release.release = *number;
    release.version = *version;
    release.version_text = *version_text;
    release.date = *date_text;
    release.from = *from;
    release.rollout = rollout;
    release.page = std::move(page);
    return true;
}

} // namespace

std::optional<Feed> read_feed(std::span<const uint8_t> bytes, std::string* error) {
    json::JsonError json_error;
    const std::optional<json::Json> root = json::parse_json(bytes_as_text(bytes), json_error);
    if (!root) {
        set_error(error, "the feed is not JSON");
        return std::nullopt;
    }
    if (root->type() != json::JsonType::object) {
        set_error(error, "the feed is not a JSON object");
        return std::nullopt;
    }
    if (!names_unique(*root, error, false))
        return std::nullopt;

    const json::Json* format_member = root->find("feed");
    const std::optional<int64_t> format =
        format_member != nullptr ? format_member->integer() : std::nullopt;
    if (!format || *format != feed_format) {
        set_error(error, "feed is not 1");
        return std::nullopt;
    }

    const json::Json* channel_member = root->find("channel");
    const std::string* channel_text =
        channel_member != nullptr ? channel_member->string() : nullptr;
    if (channel_text == nullptr) {
        set_error(error, "channel is missing");
        return std::nullopt;
    }
    if (!channel_name(*channel_text)) {
        set_error(error, "channel is not lower-case letters and digits");
        return std::nullopt;
    }

    std::optional<oamod::EngineVersion> online_min;
    if (const json::Json* online_member = root->find("online_min")) {
        const std::string* online_text = online_member->string();
        const std::optional<oamod::EngineVersion> parsed =
            online_text != nullptr ? oamod::parse_engine_version(*online_text) : std::nullopt;
        if (!parsed) {
            set_error(error, "online_min is not a version");
            return std::nullopt;
        }
        online_min = *parsed;
    }

    const json::Json* releases_member = root->find("releases");
    if (releases_member == nullptr || releases_member->type() != json::JsonType::array) {
        set_error(error, "releases is not a list");
        return std::nullopt;
    }
    const auto elements = releases_member->elements();
    if (elements.empty()) {
        set_error(error, "releases is empty");
        return std::nullopt;
    }
    if (elements.size() > max_releases) {
        set_error(error, "releases lists more than 32");
        return std::nullopt;
    }

    Feed feed;
    feed.channel = *channel_text;
    feed.online_min = online_min;
    for (const json::Json& element : elements) {
        Release release;
        if (!read_release(element, release, error))
            return std::nullopt;
        feed.releases.push_back(std::move(release));
    }
    for (std::size_t index = 0; index < feed.releases.size(); ++index) {
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            if (feed.releases[index].release == feed.releases[earlier].release) {
                set_error(error, "two releases share a release number");
                return std::nullopt;
            }
        }
    }
    std::stable_sort(
        feed.releases.begin(), feed.releases.end(), [](const Release& left, const Release& right) {
            return left.release > right.release;
        }
    );
    return feed;
}

} // namespace oa::data::engine_feed
