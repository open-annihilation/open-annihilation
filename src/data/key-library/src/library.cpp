// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The installed-layout list: each real directory's oakeys.yaml, and its
// origin record when that record reads. The player's own changes are written
// under temporary names and renamed over the old files.

#include "oa/data/key_library/library.hpp"

#include "oa/formats/oamod.hpp"
#include "oa/formats/oakeys/manifest.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace oa::data::key_library {

namespace fs = std::filesystem;
namespace keys = oa::formats::oakeys;
namespace oamod = oa::formats::oamod;

namespace {

/// The most bytes of an origin record. A larger file is not a record.
constexpr std::size_t origin_most_bytes = 4096;

/// The origin record's name, at the top of an installed folder.
constexpr std::string_view origin_file_name = ".oa-origin.yaml";

/// Returns a path's generic UTF-8 spelling.
///
/// @param path the path
/// @return its UTF-8
std::string utf8_of(const fs::path& path) {
    const std::u8string text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

/// Returns a text with its ASCII letters lowered.
///
/// @param text the text
/// @return the text
std::string folded(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const unsigned char byte : text) {
        const unsigned char lower =
            byte >= 'A' && byte <= 'Z' ? static_cast<unsigned char>(byte - 'A' + 'a') : byte;
        out.push_back(static_cast<char>(lower));
    }
    return out;
}

/// How a bounded read ended.
enum class Bound : uint8_t {
    read,       ///< the file was read, and it fits
    too_large,  ///< the file is larger than the bound
    unreadable, ///< the file did not open
};

/// Reads a file up to one byte past a bound, so a larger file is recognised
/// without giving the extra byte to a reader; a file whose size is already
/// over the bound is refused without being opened.
///
/// @param file the file
/// @param most the most bytes that still count as read
/// @param[out] bytes the file, when it fits
/// @return how the read ended
Bound read_bounded(const fs::path& file, std::size_t most, std::vector<uint8_t>& bytes) {
    std::error_code size_error;
    const uintmax_t size = fs::file_size(file, size_error);
    if (!size_error && size > most)
        return Bound::too_large;
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return Bound::unreadable;
    bytes.clear();
    for (std::istreambuf_iterator<char> at{in}, end; at != end && bytes.size() <= most; ++at)
        bytes.push_back(static_cast<uint8_t>(*at));
    if (!in && !in.eof())
        return Bound::unreadable;
    if (bytes.size() > most)
        return Bound::too_large;
    return Bound::read;
}

/// Finds a regular file in a folder by name, without ASCII case, skipping links.
///
/// @param folder the folder
/// @param wanted the name
/// @return the file; nothing when the folder cannot be listed or has no such file
std::optional<fs::path> find_file(const fs::path& folder, std::string_view wanted) {
    const std::string folded_wanted = folded(wanted);
    std::optional<fs::path> found;
    for (const fs::directory_entry& entry : fs::directory_iterator(folder)) {
        std::error_code status_error;
        const fs::file_status status = fs::symlink_status(entry.path(), status_error);
        if (status_error || fs::is_symlink(status) || !fs::is_regular_file(status))
            continue;
        if (folded(utf8_of(entry.path().filename())) != folded_wanted)
            continue;
        found = entry.path();
        break;
    }
    return found;
}

/// Tells whether a key is one an origin record uses.
///
/// @param key the key
/// @return true for oa-origin, origin, registry, catalogue-id, release, sha256
///         and installed
bool known_origin_key(std::string_view key) noexcept {
    return key == "oa-origin" || key == "origin" || key == "registry" || key == "catalogue-id" ||
           key == "release" || key == "sha256" || key == "installed";
}

/// Reads an origin from a record that is strict YAML of the known keys.
///
/// A key of the wrong type, a key the record does not use, or an origin other
/// than file, catalogue or own leaves the origin as file. Missing keys stay
/// unread. A record that does not read leaves the origin as file.
///
/// @param bytes the record, already within origin_most_bytes
/// @return the origin the record names, or file when it does not read
Origin read_origin(const std::vector<uint8_t>& bytes) {
    oamod::Node root;
    oamod::ReadError read_error{};
    if (!oamod::read_document(bytes, root, read_error) || root.kind != oamod::NodeKind::mapping)
        return Origin::file;
    std::string origin_kind;
    for (const oamod::Node& entry : root.children) {
        if (entry.key.kind != oamod::KeyKind::string || !known_origin_key(entry.key.text))
            return Origin::file;
        const std::string_view key = entry.key.text;
        if (key == "oa-origin") {
            int64_t version = 0;
            if (entry.kind != oamod::NodeKind::number ||
                !oamod::integer_value(entry.number, version))
                return Origin::file;
        } else if (key == "origin") {
            if (entry.kind != oamod::NodeKind::string)
                return Origin::file;
            origin_kind = entry.text;
        } else if (key == "release") {
            int64_t release = 0;
            if (entry.kind != oamod::NodeKind::number ||
                !oamod::integer_value(entry.number, release))
                return Origin::file;
        } else if (entry.kind != oamod::NodeKind::string) {
            return Origin::file;
        }
    }
    if (origin_kind == "catalogue")
        return Origin::catalogue;
    if (origin_kind == "own")
        return Origin::own;
    return Origin::file;
}

/// Reads one layout's manifest and origin record.
///
/// @param folder the layout's folder
/// @param name the folder's name
/// @param[in,out] problems receives why the folder was left out
/// @return the layout; nothing when its manifest does not read
std::optional<Installed>
read_layout(const fs::path& folder, const std::string& name, std::vector<std::string>& problems) {
    const std::optional<fs::path> manifest_path = find_file(folder, keys::manifest_file);
    if (!manifest_path) {
        problems.push_back(name + ": oakeys.yaml: it cannot be read");
        return std::nullopt;
    }
    std::vector<uint8_t> bytes;
    const Bound manifest_read = read_bounded(*manifest_path, oamod::max_input_bytes, bytes);
    if (manifest_read == Bound::too_large) {
        problems.push_back(name + ": oakeys.yaml: it is larger than 256 KiB");
        return std::nullopt;
    }
    if (manifest_read != Bound::read) {
        problems.push_back(name + ": oakeys.yaml: it cannot be read");
        return std::nullopt;
    }
    keys::Manifest manifest;
    std::vector<keys::Problem> manifest_problems;
    if (!keys::read_manifest(bytes, manifest, manifest_problems)) {
        if (manifest_problems.empty())
            problems.push_back(name + ": oakeys.yaml: it cannot be read");
        for (const keys::Problem& problem : manifest_problems)
            problems.push_back(name + ": " + keys::describe(problem));
        return std::nullopt;
    }
    Installed installed;
    installed.folder = folder;
    installed.manifest = std::move(manifest);
    const std::optional<fs::path> origin_path = find_file(folder, origin_file_name);
    if (!origin_path)
        return installed;
    std::vector<uint8_t> origin_bytes;
    if (read_bounded(*origin_path, origin_most_bytes, origin_bytes) != Bound::read)
        return installed;
    installed.origin = read_origin(origin_bytes);
    return installed;
}

/// A folder of Keys, in the order its name sorts.
struct NamedFolder {
    std::string name{};
    fs::path path{};
};

/// Keeps one layout of an id: the folder named as the id, or else the first.
///
/// @param[in,out] layouts the layouts kept so far, in walk order
/// @param[in,out] problems receives the folder that was not kept
/// @param incoming the layout just read
void keep_one(
    std::vector<Installed>& layouts, std::vector<std::string>& problems, Installed incoming
) {
    const auto same_id = [&](const Installed& kept) {
        return kept.manifest.id == incoming.manifest.id;
    };
    const auto found = std::find_if(layouts.begin(), layouts.end(), same_id);
    if (found == layouts.end()) {
        layouts.push_back(std::move(incoming));
        return;
    }
    const std::string incoming_name = utf8_of(incoming.folder.filename());
    const std::string kept_name = utf8_of(found->folder.filename());
    const bool incoming_named = incoming_name == incoming.manifest.id;
    const bool kept_named = kept_name == found->manifest.id;
    if (incoming_named && !kept_named) {
        problems.push_back(
            kept_name + ": it has the same id " + found->manifest.id + " as " + incoming_name
        );
        *found = std::move(incoming);
        return;
    }
    problems.push_back(
        incoming_name + ": it has the same id " + incoming.manifest.id + " as " + kept_name
    );
}

/// Tells whether a base id can be one path component.
///
/// @param base_id the layout id
/// @return false when it is empty, is `..`, or holds a separator
bool one_folder_name(std::string_view base_id) {
    if (base_id.empty() || base_id == ".." || base_id == ".")
        return false;
    return base_id.find('/') == std::string_view::npos &&
           base_id.find('\\') == std::string_view::npos &&
           base_id.find(static_cast<char>(fs::path::preferred_separator)) == std::string_view::npos;
}

/// Writes bytes to a new file.
///
/// @param file the file
/// @param text its text
/// @param[out] error why the write failed
/// @return true when the file holds the text
bool write_text(const fs::path& file, std::string_view text, std::string& error) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = utf8_of(file.filename()) + ": it cannot be written";
        return false;
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.close();
    if (!out) {
        error = utf8_of(file.filename()) + ": it cannot be written";
        return false;
    }
    return true;
}

/// Replaces `to` with `from`. On Windows a plain rename leaves the old file.
///
/// @param from the temporary file
/// @param to the file it replaces
/// @param[out] error why the replace failed
/// @return true when `to` is the renamed file
bool replace_file(const fs::path& from, const fs::path& to, std::string& error) {
#ifdef _WIN32
    if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING) == 0) {
        error = utf8_of(to.filename()) + ": it cannot be replaced";
        return false;
    }
    return true;
#else
    std::error_code rename_error;
    fs::rename(from, to, rename_error);
    if (rename_error) {
        error = utf8_of(to.filename()) + ": it cannot be replaced";
        return false;
    }
    return true;
#endif
}

/// Removes a temporary file. A missing file is not a failure.
///
/// @param file the temporary file
void remove_temporary(const fs::path& file) {
    std::error_code ignored;
    fs::remove(file, ignored);
}

} // namespace

std::string own_folder_name(std::string_view base_id) {
    return std::string("own-") + std::string(base_id);
}

const Installed* find(const Listing& listing, std::string_view id) noexcept {
    for (const Installed& installed : listing.layouts)
        if (installed.manifest.id == id)
            return &installed;
    return nullptr;
}

Listing list_installed(const fs::path& keys_folder) {
    Listing listing;
    std::error_code exists_error;
    if (!fs::exists(keys_folder, exists_error) || exists_error)
        return listing;
    if (!fs::is_directory(keys_folder, exists_error) || exists_error)
        return listing;
    try {
        std::vector<NamedFolder> folders;
        for (const fs::directory_entry& entry : fs::directory_iterator(keys_folder)) {
            std::error_code status_error;
            const fs::file_status status = fs::symlink_status(entry.path(), status_error);
            if (status_error || !fs::is_directory(status))
                continue;
            const std::string name = utf8_of(entry.path().filename());
            if (name.empty() || name.front() == '.')
                continue;
            folders.push_back(NamedFolder{name, entry.path()});
        }
        std::sort(
            folders.begin(), folders.end(), [](const NamedFolder& one, const NamedFolder& other) {
                return one.name < other.name;
            }
        );
        for (const NamedFolder& named : folders) {
            try {
                std::optional<Installed> installed =
                    read_layout(named.path, named.name, listing.problems);
                if (installed)
                    keep_one(listing.layouts, listing.problems, std::move(*installed));
            } catch (const std::exception&) {
                listing.problems.push_back(named.name + ": oakeys.yaml: it cannot be read");
            }
        }
    } catch (const std::exception&) {
        listing.problems.push_back(utf8_of(keys_folder.filename()) + ": it cannot be listed");
    }
    std::sort(
        listing.layouts.begin(),
        listing.layouts.end(),
        [](const Installed& one, const Installed& other) {
            return one.manifest.id < other.manifest.id;
        }
    );
    return listing;
}

bool write_own(
    const fs::path& keys_folder,
    std::string_view base_id,
    const keys::Manifest& manifest,
    std::string& error
) {
    error.clear();
    if (!one_folder_name(base_id)) {
        error = "the layout id is not a folder name";
        return false;
    }
    const std::string folder_name = own_folder_name(base_id);
    const fs::path folder = keys_folder / folder_name;
    std::error_code create_error;
    fs::create_directories(folder, create_error);
    if (create_error) {
        error = folder_name + ": it cannot be made";
        return false;
    }
    std::error_code status_error;
    const fs::file_status status = fs::symlink_status(folder, status_error);
    if (status_error || fs::is_symlink(status) || !fs::is_directory(status) ||
        utf8_of(folder.filename()) != folder_name) {
        error = folder_name + ": it is not a folder of Keys";
        return false;
    }
    const fs::path manifest_temp = folder / "oakeys.yaml.tmp";
    const fs::path origin_temp = folder / ".oa-origin.yaml.tmp";
    const fs::path manifest_file = folder / std::string(keys::manifest_file);
    const fs::path origin_file = folder / std::string(origin_file_name);
    const std::string manifest_text = keys::write_manifest(manifest);
    const std::string origin_text = "oa-origin: 1\norigin: own\n";
    if (!write_text(manifest_temp, manifest_text, error) ||
        !write_text(origin_temp, origin_text, error)) {
        remove_temporary(manifest_temp);
        remove_temporary(origin_temp);
        return false;
    }
    if (!replace_file(manifest_temp, manifest_file, error)) {
        remove_temporary(manifest_temp);
        remove_temporary(origin_temp);
        return false;
    }
    if (!replace_file(origin_temp, origin_file, error)) {
        remove_temporary(origin_temp);
        return false;
    }
    return true;
}

} // namespace oa::data::key_library
