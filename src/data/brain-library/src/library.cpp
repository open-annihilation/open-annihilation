// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The installed-brain list: each real directory's oabrain.yaml, and its
// origin record when that record reads. Nothing else in the folder is
// opened. A network file the manifest names is measured, not read.

#include "oa/data/brain_library/library.hpp"

#include "oa/formats/oamod.hpp"
#include "oa/formats/oabrain/manifest.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace oa::data::brain_library {

namespace fs = std::filesystem;
namespace brain = oa::formats::oabrain;
namespace oamod = oa::formats::oamod;

namespace {

/// The most bytes of an origin record. The installer's limit; a larger file
/// is not a record. This module does not link the installer.
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
    // A file already over the bound is never opened; the bounded read below
    // still guards against one that grows after its size was read.
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
        std::error_code error;
        const fs::file_status status = fs::symlink_status(entry.path(), error);
        if (error || fs::is_symlink(status) || !fs::is_regular_file(status))
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

/// Reads origin facts from a record that is strict YAML of the known keys.
///
/// A key of the wrong type, a key the record does not use, or an origin other
/// than file or catalogue leaves every fact empty. A file origin keeps the
/// SHA-256 only. Missing keys stay empty, or 0.
///
/// @param bytes the record, already within origin_most_bytes
/// @param[in,out] installed receives the facts when the record reads
void read_origin_facts(const std::vector<uint8_t>& bytes, InstalledBrain& installed) {
    oamod::Node root;
    oamod::ReadError error{};
    if (!oamod::read_document(bytes, root, error) || root.kind != oamod::NodeKind::mapping)
        return;
    std::string origin_kind;
    std::string sha;
    std::string registry;
    std::string catalogue_id;
    int64_t release = 0;
    for (const oamod::Node& entry : root.children) {
        if (entry.key.kind != oamod::KeyKind::string || !known_origin_key(entry.key.text))
            return;
        const std::string_view key = entry.key.text;
        if (key == "oa-origin") {
            int64_t version = 0;
            if (entry.kind != oamod::NodeKind::number ||
                !oamod::integer_value(entry.number, version))
                return;
        } else if (key == "origin") {
            if (entry.kind != oamod::NodeKind::string)
                return;
            origin_kind = entry.text;
        } else if (key == "release") {
            if (entry.kind != oamod::NodeKind::number ||
                !oamod::integer_value(entry.number, release))
                return;
        } else if (entry.kind != oamod::NodeKind::string) {
            return;
        } else if (key == "sha256") {
            sha = entry.text;
        } else if (key == "registry") {
            registry = entry.text;
        } else if (key == "catalogue-id") {
            catalogue_id = entry.text;
        }
    }
    if (origin_kind == "catalogue") {
        installed.package_sha256 = std::move(sha);
        installed.registry = std::move(registry);
        installed.catalogue_id = std::move(catalogue_id);
        installed.release = release;
        return;
    }
    if (origin_kind == "file") {
        installed.package_sha256 = std::move(sha);
        installed.registry.clear();
        installed.catalogue_id.clear();
        installed.release = 0;
    }
}

/// Returns a regular file inside a folder for a relative path, or nothing
/// when the path leaves the folder, names a link, or is not a regular file.
///
/// The path is split on '/'. '.' is skipped. An empty part, '..', or a path
/// that starts at the root is refused, so the file stays inside the folder.
/// Each part is checked without following a link.
///
/// @param folder the brain's folder
/// @param relative the path the manifest names
/// @return the file; nothing when it cannot be measured
std::optional<fs::path> file_inside(const fs::path& folder, std::string_view relative) {
    if (relative.empty() || relative.front() == '/' || relative.front() == '\\')
        return std::nullopt;
    fs::path at = folder;
    std::size_t start = 0;
    bool any = false;
    while (start <= relative.size()) {
        const std::size_t cut = relative.find('/', start);
        const std::size_t end = cut == std::string_view::npos ? relative.size() : cut;
        const std::string_view part = relative.substr(start, end - start);
        if (part.empty() || part == "..")
            return std::nullopt;
        const bool last = cut == std::string_view::npos;
        if (part != ".") {
            at /= fs::path(std::u8string(part.begin(), part.end()));
            any = true;
            std::error_code error;
            const fs::file_status status = fs::symlink_status(at, error);
            if (error || fs::is_symlink(status))
                return std::nullopt;
            if (last) {
                if (!fs::is_regular_file(status))
                    return std::nullopt;
            } else if (!fs::is_directory(status)) {
                return std::nullopt;
            }
        }
        if (last)
            break;
        start = cut + 1;
    }
    if (!any)
        return std::nullopt;
    return at;
}

/// Adds the sizes of the network files a manifest names that exist.
///
/// A missing file, a link, or a path that leaves the folder adds nothing.
/// The files are not opened. The sum stops at the largest uint64_t.
///
/// @param folder the brain's folder
/// @param manifest the manifest
/// @return the sizes added together
uint64_t network_bytes(const fs::path& folder, const brain::Manifest& manifest) {
    uint64_t total = 0;
    for (const brain::Network& network : manifest.networks) {
        const std::optional<fs::path> file = file_inside(folder, network.file);
        if (!file)
            continue;
        std::error_code error;
        const uintmax_t size = fs::file_size(*file, error);
        if (error)
            continue;
        if (size > std::numeric_limits<uint64_t>::max() - total)
            return std::numeric_limits<uint64_t>::max();
        total += static_cast<uint64_t>(size);
    }
    return total;
}

/// Reads one brain's manifest and origin record.
///
/// @param folder the brain's folder
/// @param name the folder's name
/// @param[in,out] problems receives why the folder was left out
/// @return the brain; nothing when its manifest does not read
std::optional<InstalledBrain>
read_brain(const fs::path& folder, const std::string& name, std::vector<std::string>& problems) {
    const std::optional<fs::path> manifest_path = find_file(folder, brain::manifest_file);
    if (!manifest_path) {
        problems.push_back(name + ": oabrain.yaml: it cannot be read");
        return std::nullopt;
    }
    std::vector<uint8_t> bytes;
    const Bound manifest_read = read_bounded(*manifest_path, oamod::max_input_bytes, bytes);
    if (manifest_read == Bound::too_large) {
        problems.push_back(name + ": oabrain.yaml: it is larger than 256 KiB");
        return std::nullopt;
    }
    if (manifest_read != Bound::read) {
        problems.push_back(name + ": oabrain.yaml: it cannot be read");
        return std::nullopt;
    }
    brain::Manifest manifest;
    std::vector<brain::Problem> manifest_problems;
    if (!brain::read_manifest(bytes, manifest, manifest_problems)) {
        if (manifest_problems.empty())
            problems.push_back(name + ": oabrain.yaml: it cannot be read");
        for (const brain::Problem& problem : manifest_problems)
            problems.push_back(name + ": " + brain::describe(problem));
        return std::nullopt;
    }
    InstalledBrain installed;
    installed.folder_name = name;
    installed.folder = folder;
    installed.network_bytes = network_bytes(folder, manifest);
    installed.manifest = std::move(manifest);
    const std::optional<fs::path> origin_path = find_file(folder, origin_file_name);
    if (!origin_path)
        return installed;
    std::vector<uint8_t> origin_bytes;
    if (read_bounded(*origin_path, origin_most_bytes, origin_bytes) != Bound::read)
        return installed;
    read_origin_facts(origin_bytes, installed);
    return installed;
}

/// A folder of Brains, in the order its name sorts.
struct NamedFolder {
    std::string name{};
    fs::path path{};
};

/// Keeps one brain of an id: the folder named as the id, or else the first.
///
/// @param[in,out] brains the brains kept so far, in walk order
/// @param[in,out] problems receives the folder that was not kept
/// @param incoming the brain just read
void keep_one(
    std::vector<InstalledBrain>& brains, std::vector<std::string>& problems, InstalledBrain incoming
) {
    const auto same_id = [&](const InstalledBrain& kept) {
        return kept.manifest.id == incoming.manifest.id;
    };
    const auto found = std::find_if(brains.begin(), brains.end(), same_id);
    if (found == brains.end()) {
        brains.push_back(std::move(incoming));
        return;
    }
    const bool incoming_named = incoming.folder_name == incoming.manifest.id;
    const bool kept_named = found->folder_name == found->manifest.id;
    if (incoming_named && !kept_named) {
        problems.push_back(
            found->folder_name + ": it has the same id " + found->manifest.id + " as " +
            incoming.folder_name
        );
        *found = std::move(incoming);
        return;
    }
    problems.push_back(
        incoming.folder_name + ": it has the same id " + incoming.manifest.id + " as " +
        found->folder_name
    );
}

} // namespace

const InstalledBrain* find(const Listing& listing, std::string_view id) noexcept {
    for (const InstalledBrain& installed : listing.brains)
        if (installed.manifest.id == id)
            return &installed;
    return nullptr;
}

Listing list_installed(const fs::path& brains_folder) {
    Listing listing;
    std::error_code error;
    if (!fs::exists(brains_folder, error) || error)
        return listing;
    if (!fs::is_directory(brains_folder, error) || error)
        return listing;
    try {
        std::vector<NamedFolder> folders;
        for (const fs::directory_entry& entry : fs::directory_iterator(brains_folder)) {
            std::error_code status_error;
            const fs::file_status status = fs::symlink_status(entry.path(), status_error);
            // symlink_status does not follow a link, so a link is not a directory.
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
                std::optional<InstalledBrain> installed =
                    read_brain(named.path, named.name, listing.problems);
                if (installed)
                    keep_one(listing.brains, listing.problems, std::move(*installed));
            } catch (const std::exception&) {
                listing.problems.push_back(named.name + ": oabrain.yaml: it cannot be read");
            }
        }
    } catch (const std::exception&) {
        listing.problems.push_back(utf8_of(brains_folder.filename()) + ": it cannot be listed");
    }
    std::sort(
        listing.brains.begin(),
        listing.brains.end(),
        [](const InstalledBrain& one, const InstalledBrain& other) {
            return one.manifest.id < other.manifest.id;
        }
    );
    return listing;
}

} // namespace oa::data::brain_library
