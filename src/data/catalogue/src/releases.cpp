// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Reading a releases list: the size and the digest first, then each entry
// on its own. A bad entry is skipped. A file that is not the one the
// catalogue named leaves the list already held where it was.

#include "oa/data/catalogue/releases.hpp"

#include "oa/base/sha256.hpp"
#include "oa/data/registry/descriptor.hpp"
#include "oa/formats/json.hpp"
#include "oa/formats/oamod/package_keys.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::data::catalogue {
namespace {

namespace json = formats::json;
namespace oamod = formats::oamod;
namespace sha = base::sha256;

/// The most bytes one package may name. 2^32 itself is allowed.
constexpr int64_t max_package_bytes = int64_t{1} << 32;

/// The highest release number. 2^31-1.
constexpr int64_t max_release = 2147483647;

/// The highest packaging revision a list entry may name.
constexpr int64_t max_revision = 65535;

/// Reads `kind` as one of the package kinds.
bool parse_kind(std::string_view text, Kind& kind) noexcept {
    if (text == "oalang") {
        kind = Kind::oalang;
        return true;
    }
    if (text == "oamod") {
        kind = Kind::oamod;
        return true;
    }
    if (text == "oamap") {
        kind = Kind::oamap;
        return true;
    }
    if (text == "expansion") {
        kind = Kind::expansion;
        return true;
    }
    if (text == "oamut") {
        kind = Kind::oamut;
        return true;
    }
    if (text == "oabrain") {
        kind = Kind::oabrain;
        return true;
    }
    if (text == "oakeys") {
        kind = Kind::oakeys;
        return true;
    }
    return false;
}

/// Reports whether `text` is 64 lower-case hexadecimal digits.
bool lower_hex(std::string_view text) noexcept {
    if (text.size() != sha::hex_size)
        return false;
    for (const char value : text) {
        const bool digit = value >= '0' && value <= '9';
        const bool hex = value >= 'a' && value <= 'f';
        if (!digit && !hex)
            return false;
    }
    return true;
}

/// The last path segment of a reference.
std::string_view last_segment(std::string_view path) noexcept {
    const auto slash = path.rfind('/');
    if (slash == std::string_view::npos)
        return path;
    return path.substr(slash + 1);
}

/// Reports whether `file`'s last segment is the package's hash and kind.
bool file_named(std::string_view file, const sha::Digest& digest, Kind kind) {
    const auto hex = sha::to_hex(digest);
    std::string expected(hex.data(), hex.size());
    expected += '.';
    expected += package_extension(kind);
    return last_segment(file) == expected;
}

/// Reports whether an object names each of its members once.
bool names_once(const json::Json& object) {
    const auto names = object.names();
    for (std::size_t index = 0; index < names.size(); ++index) {
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            if (names[earlier] == names[index])
                return false;
        }
    }
    return true;
}

/// Keeps why one entry was skipped, until max_problems reasons are kept.
void keep_problem(ReleasesList& list, std::size_t index, const std::string& reason) {
    if (list.problems.size() >= max_problems)
        return;
    list.problems.push_back("packages[" + std::to_string(index) + "]: " + reason);
}

/// Reads one release. The first broken rule is the reason the entry is left out.
bool read_entry(const json::Json& value, ReleaseEntry& entry, std::string& reason) {
    if (value.type() != json::JsonType::object) {
        reason = "a package is not an object";
        return false;
    }
    if (!names_once(value)) {
        reason = "a member is named twice";
        return false;
    }
    const json::Json* kind_value = value.find("kind");
    const std::string* kind_text = kind_value ? kind_value->string() : nullptr;
    Kind kind = Kind::oamod;
    if (!kind_text || !parse_kind(*kind_text, kind)) {
        reason = "unknown kind";
        return false;
    }
    entry.kind = kind;
    const json::Json* id_value = value.find("id");
    const std::string* id_text = id_value ? id_value->string() : nullptr;
    if (!id_text || !valid_package_key(kind, *id_text)) {
        reason = "id is not a package key";
        return false;
    }
    entry.id = *id_text;
    const json::Json* name_value = value.find("name");
    const std::string* name_text = name_value ? name_value->string() : nullptr;
    if (!name_text || name_text->empty() || name_text->size() > 64) {
        reason = "name is not 1 to 64 bytes";
        return false;
    }
    entry.name = *name_text;
    const json::Json* version_value = value.find("version");
    const std::string* version_text = version_value ? version_value->string() : nullptr;
    const bool version_needed = kind == Kind::oamod || kind == Kind::expansion ||
                                kind == Kind::oamut || kind == Kind::oabrain ||
                                kind == Kind::oakeys;
    if (version_needed) {
        if (!version_text || version_text->empty() || version_text->size() > 32) {
            reason = "version is not 1 to 32 bytes";
            return false;
        }
        entry.version = *version_text;
    } else if (version_value) {
        if (!version_text || version_text->empty() || version_text->size() > 32) {
            reason = "version is not 1 to 32 bytes";
            return false;
        }
        entry.version = *version_text;
    }
    const json::Json* revision = value.find("revision");
    if (revision) {
        const auto number = revision->integer();
        if (!number || *number < 1 || *number > max_revision) {
            reason = "revision is not a whole number from 1 to 65535";
            return false;
        }
        entry.revision = static_cast<int32_t>(*number);
    }
    const json::Json* release = value.find("release");
    const auto release_number = release ? release->integer() : std::nullopt;
    if (!release_number || *release_number < 1 || *release_number > max_release) {
        reason = "release is not a whole number from 1 to 2147483647";
        return false;
    }
    entry.release = *release_number;
    const json::Json* published = value.find("published");
    if (published) {
        const std::string* text = published->string();
        const bool day = text && parse_day(*text).has_value();
        const bool time = text && parse_utc_time(*text).has_value();
        if (!text || (!day && !time)) {
            reason = "published is not a day or a UTC time";
            return false;
        }
        entry.published = *text;
    }
    const json::Json* requirement = value.find("requires");
    if (requirement) {
        if (requirement->type() != json::JsonType::object || !names_once(*requirement)) {
            reason = "requires is not an object";
            return false;
        }
        if (const json::Json* engine = requirement->find("engine")) {
            const std::string* text = engine->string();
            std::string range_error;
            const auto range = text ? oamod::parse_engine_range(*text, &range_error) : std::nullopt;
            if (!range) {
                reason = "requires.engine is not an engine requirement";
                return false;
            }
            entry.requires_engine_text = *text;
            entry.requires_engine = *range;
        }
    }
    const json::Json* size = value.find("size");
    const auto size_number = size ? size->integer() : std::nullopt;
    if (!size_number || *size_number < 1 || *size_number > max_package_bytes) {
        reason = "size is not a whole number of bytes from 1 to 4294967296";
        return false;
    }
    entry.size = static_cast<uint64_t>(*size_number);
    const json::Json* hash = value.find("sha256");
    const std::string* hash_text = hash ? hash->string() : nullptr;
    if (!hash_text || !lower_hex(*hash_text)) {
        reason = "sha256 is not 64 lower-case hex digits";
        return false;
    }
    entry.sha256 = *sha::parse_hex(*hash_text);
    const json::Json* file = value.find("file");
    const std::string* file_text = file ? file->string() : nullptr;
    if (!file_text || !valid_reference(*file_text)) {
        reason = "file is not a reference";
        return false;
    }
    if (!file_named(*file_text, entry.sha256, kind)) {
        reason = "file not named by sha256";
        return false;
    }
    entry.file = *file_text;
    return true;
}

/// Reads a list whose bytes already match the member. False when it is not a list.
bool read_list(std::span<const uint8_t> bytes, ReleasesList& out, bool* ignored) {
    if (ignored)
        *ignored = false;
    json::JsonError json_error;
    const std::string_view text =
        bytes.empty() ? std::string_view{}
                      : std::string_view{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    const std::optional<json::Json> root = json::parse_json(text, json_error);
    if (!root || root->type() != json::JsonType::object || !names_once(*root))
        return false;
    const json::Json* version = root->find("releases");
    const auto version_number = version ? version->integer() : std::nullopt;
    if (!version_number)
        return false;
    if (*version_number != releases_version) {
        if (ignored)
            *ignored = true;
        return false;
    }
    const json::Json* registry_value = root->find("registry");
    const std::string* registry_text = registry_value ? registry_value->string() : nullptr;
    if (!registry_text || !registry::valid_registry_id(*registry_text))
        return false;
    ReleasesList loaded;
    loaded.registry = *registry_text;
    const json::Json* sequence = root->find("sequence");
    const auto sequence_number = sequence ? sequence->integer() : std::nullopt;
    if (!sequence_number || *sequence_number < 0)
        return false;
    loaded.sequence = *sequence_number;
    const json::Json* generated = root->find("generated");
    const std::string* generated_text = generated ? generated->string() : nullptr;
    const auto generated_seconds = generated_text ? parse_utc_time(*generated_text) : std::nullopt;
    if (!generated_seconds)
        return false;
    loaded.generated_text = *generated_text;
    loaded.generated = *generated_seconds;
    const json::Json* packages = root->find("packages");
    if (!packages || packages->type() != json::JsonType::array)
        return false;
    if (packages->elements().size() > max_packages)
        return false;
    for (std::size_t index = 0; index < packages->elements().size(); ++index) {
        ReleaseEntry entry;
        std::string reason;
        if (!read_entry(packages->elements()[index], entry, reason)) {
            keep_problem(loaded, index, reason);
            continue;
        }
        loaded.packages.push_back(std::move(entry));
    }
    out = std::move(loaded);
    return true;
}

} // namespace

HeldReleases take_releases(
    const ReleasesRef& member, std::span<const uint8_t> bytes, HeldReleases held, ReleasesTake* how
) {
    const auto report = [&](ReleasesTake take) {
        if (how)
            *how = take;
        return held;
    };
    if (bytes.size() != member.size)
        return report(ReleasesTake::mismatch);
    if (sha::digest_of(bytes) != member.sha256)
        return report(ReleasesTake::mismatch);
    bool ignored = false;
    ReleasesList loaded;
    if (!read_list(bytes, loaded, &ignored))
        return report(ignored ? ReleasesTake::ignored : ReleasesTake::mismatch);
    HeldReleases taken;
    taken.digest = member.sha256;
    taken.list = std::move(loaded);
    if (how)
        *how = ReleasesTake::taken;
    return taken;
}

} // namespace oa::data::catalogue
