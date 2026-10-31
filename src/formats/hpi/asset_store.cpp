// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/formats/hpi.hpp"

#include "oa/base/threads.hpp"
#include "oa/formats/zip/stream.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace oa {
namespace {

constexpr std::size_t kMaxPathLength = 4096;
constexpr uint32_t kSeekToEnd = 0xFFFFFFFFU;
// Search spec rewritten to "*" when a find names it as its basename.
constexpr std::string_view kAllFilesPattern = "*.*";
constexpr std::string_view kCurrentDirectory = ".";
constexpr std::string_view kParentDirectory = "..";
constexpr std::string_view kDiscoveryGp3 = ".GP3";
constexpr std::string_view kDiscoveryGp3Prefix = "rev";
// Mount-time mark on an archive file that a loose file of the same path hides.
constexpr uint8_t kShadowedByLoose = 0x02;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

/// Returns a path's UTF-8 spelling: a name outside the system's code page
/// has no narrow spelling on Windows.
///
/// @param path the path
/// @return its UTF-8 spelling
std::string utf8_text(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

/// Throws the error of a read from a mounted archive, naming the entry and
/// the archive.
///
/// @param error the archive's error
/// @param archive_path host path of the archive read
/// @param node the entry read
[[noreturn]] void fail_entry_read(
    const base::bytes::DecodeError& error,
    const std::filesystem::path& archive_path,
    const ArchiveNode& node
) {
    // A chunk that fails to decode carries its SQUASHERR_* name.
    const std::string what = error.detail != 0
                                 ? "HPI decompression error " + std::string(error.message)
                                 : std::string(error.message);
    fail(
        what + " (" + node.name + ", length " + std::to_string(node.size) + ", at byte " +
        std::to_string(error.offset) + " of " + utf8_text(archive_path.filename()) + ")"
    );
}

char ascii_lower(char value) noexcept {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

char ascii_upper(char value) noexcept {
    return value >= 'a' && value <= 'z' ? static_cast<char>(value - ('a' - 'A')) : value;
}

// NTFS directory order: names compared after ASCII upper-casing.
bool ntfs_less(const std::string& a, const std::string& b) noexcept {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
        return static_cast<unsigned char>(ascii_upper(x)) <
               static_cast<unsigned char>(ascii_upper(y));
    });
}

// Directory-listing match: "*.*" is "*", otherwise the shared wildcard rule.
bool match_host_pattern(std::string_view name, std::string_view pattern) noexcept {
    if (pattern == kAllFilesPattern)
        pattern = "*";
    return match_wildcard(name, pattern);
}

std::string normalized_path(std::string_view path) {
    std::string result;
    result.reserve(path.size());
    bool previous_slash = true;
    for (const char raw : path) {
        const char ch = raw == '\\' ? '/' : raw;
        if (ch == '/') {
            if (!previous_slash)
                result.push_back('/');
            previous_slash = true;
            continue;
        }
        result.push_back(ascii_lower(ch));
        previous_slash = false;
    }
    if (!result.empty() && result.back() == '/')
        result.pop_back();
    return result;
}

std::size_t last_separator(std::string_view path) noexcept {
    const auto at = path.find_last_of("\\/");
    return at == std::string_view::npos ? 0 : at + 1;
}

std::string full_path_key(const std::filesystem::path& path) {
    // In UTF-8: a folder named outside the system's code page has no narrow
    // spelling on Windows.
    const auto spelled = std::filesystem::weakly_canonical(path).generic_u8string();
    std::string text(spelled.begin(), spelled.end());
    std::transform(text.begin(), text.end(), text.begin(), ascii_upper);
    return text;
}

/// Returns a host path with every link followed, or the path as given when
/// it cannot be resolved.
///
/// @param path the path
/// @return the resolved path
std::filesystem::path resolved(const std::filesystem::path& path) {
    std::error_code error;
    auto canonical = std::filesystem::weakly_canonical(path, error);
    return error ? path : canonical;
}

/// Tells whether a host path, every link followed, is a folder or lies
/// inside it, the folder's own links followed too.
///
/// The folder is matched by identity, not by how its path is spelled, so
/// the case of its letters and the system's spelling of long paths do not
/// matter.
///
/// @param root the folder
/// @param path a path found below it
/// @return true when the resolved path is the folder or lies inside it
bool stays_inside(const std::filesystem::path& root, const std::filesystem::path& path) {
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(path, error);
    if (error)
        return false;
    for (auto at = canonical;; at = at.parent_path()) {
        if (std::filesystem::equivalent(at, root, error))
            return true;
        if (!at.has_relative_path())
            return false;
    }
}

/// Tells whether a host entry, as it is now, is a plain file or folder: not
/// a link, a junction or, on Windows, any other reparse point, which the
/// system may follow elsewhere.
///
/// @param path the entry
/// @return false for a link or an entry whose kind cannot be read
bool plain_entry(const std::filesystem::path& path) noexcept {
#if defined(_WIN32)
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
#else
    std::error_code error;
    const auto own = std::filesystem::symlink_status(path, error).type();
    return !error && (own == std::filesystem::file_type::regular ||
                      own == std::filesystem::file_type::directory);
#endif
}

/// Tells whether a file a lookup found below a folder lies inside it, as
/// the file system is now.
///
/// When the file and every folder between it and the root are plain, the
/// path lies inside the root as it names it; otherwise it is resolved and
/// compared with the root, as stays_inside() does.
///
/// @param root the folder
/// @param path the file: the root joined with the names of the parts
/// @param parts how many names the path adds to the root
/// @return true when the file lies inside the folder
bool found_inside(
    const std::filesystem::path& root, const std::filesystem::path& path, std::size_t parts
) {
    auto at = path;
    for (std::size_t part = 0; part < parts; ++part, at = at.parent_path())
        if (!plain_entry(at))
            return stays_inside(root, path);
    return true;
}

std::vector<uint8_t> read_loose(const std::filesystem::path& path) {
    const auto size = std::filesystem::file_size(path);
    if (!formats::hpi::entry_size_allowed(size))
        fail("loose asset exceeds entry size limit");
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        fail("cannot open loose asset '" + utf8_text(path) + "'");
    std::vector<uint8_t> bytes(static_cast<std::size_t>(size));
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (stream.gcount() != static_cast<std::streamsize>(bytes.size()))
        fail("truncated loose asset '" + utf8_text(path) + "'");
    return bytes;
}

/// An archive file kept open for reads of its entries.
class ArchiveFile final : public ArchiveSource {
  public:

    /// Takes over an open file.
    ///
    /// @param stream the file, opened for binary reads
    /// @param size its size in bytes
    ArchiveFile(std::ifstream stream, uint64_t size) : stream_(std::move(stream)), size_(size) {}

    /// Returns the file's size when it was opened.
    ///
    /// @return the size in bytes
    [[nodiscard]] uint64_t size() const noexcept override { return size_; }

    /// Reads file bytes from an offset.
    ///
    /// @param offset file offset of the first byte
    /// @param[out] output receives up to output.size() bytes
    /// @return the count read; short past the end of the file
    std::size_t read_at(uint64_t offset, std::span<uint8_t> output) override {
        stream_.clear();
        stream_.seekg(static_cast<std::streamoff>(offset));
        if (!stream_)
            return 0;
        stream_.read(
            reinterpret_cast<char*>(output.data()), static_cast<std::streamsize>(output.size())
        );
        return static_cast<std::size_t>(stream_.gcount());
    }

  private:

    std::ifstream stream_;
    uint64_t size_{};
};

/// Opens an archive file for reads, saying why it cannot be opened.
///
/// @param path host path of the archive
/// @param[out] reason receives why the file cannot be opened
/// @return the open file, or null
std::unique_ptr<ArchiveFile>
open_archive_file(const std::filesystem::path& path, std::string& reason) {
    std::error_code status;
    const auto size = std::filesystem::file_size(path, status);
    if (status) {
        reason = "cannot stat HPI archive '" + utf8_text(path) + "': " + status.message();
        return nullptr;
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        reason = "cannot open HPI archive '" + utf8_text(path) + "'";
        return nullptr;
    }
    return std::make_unique<ArchiveFile>(std::move(stream), size);
}

struct LooseItem {
    std::string name;
    std::filesystem::path path;
    bool directory = false;
    uint32_t size = 0;
};

/// Lists a directory in NTFS order, led by the "." and ".." entries Windows
/// lists in every directory but a drive root.
std::vector<LooseItem> loose_listing(const std::filesystem::path& directory) {
    std::vector<LooseItem> items;
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error))
        return items;
    for (const auto& item : std::filesystem::directory_iterator(directory, error)) {
        LooseItem entry{
            utf8_text(item.path().filename()), item.path(), item.is_directory(error), 0
        };
        if (!entry.directory && item.is_regular_file(error)) {
            const auto size = item.file_size(error);
            entry.size = error ? 0 : static_cast<uint32_t>(std::min<uintmax_t>(size, 0xFFFFFFFFU));
        }
        items.push_back(std::move(entry));
    }
    std::sort(items.begin(), items.end(), [](const LooseItem& a, const LooseItem& b) {
        return ntfs_less(a.name, b.name);
    });
    items.insert(
        items.begin(), LooseItem{std::string(kParentDirectory), directory / "..", true, 0}
    );
    items.insert(items.begin(), LooseItem{std::string(kCurrentDirectory), directory, true, 0});
    return items;
}

/// Tells whether a name at the top of a layered folder is hidden from the
/// store: the folder a mod keeps its earlier version in (backup_folder_name).
///
/// @param folded_name the name, ASCII lower case
/// @return true for the hidden folder
bool hidden_at_top(std::string_view folded_name) noexcept {
    return folded_name == backup_folder_name;
}

/// Returns the first '/'-separated part of a folded key.
///
/// @param key the key
/// @return its first part; the key itself without a separator
std::string_view first_part(std::string_view key) noexcept {
    return key.substr(0, key.find('/'));
}

/// Tests whether a listing entry is the "." or ".." entry.
bool dot_entry(const LooseItem& item) noexcept {
    return item.name == kCurrentDirectory || item.name == kParentDirectory;
}

/// Tests whether a listing entry is an empty file. A listing also gives size
/// 0 to an entry it could not size, so the file is asked again: one that
/// cannot be sized is not taken as empty.
bool empty_file(const LooseItem& item) {
    if (item.directory || item.size != 0)
        return false;
    std::error_code error;
    const auto size = std::filesystem::file_size(item.path, error);
    return !error && size == 0;
}

/// Lists folders layered in order as one folder, in NTFS order, led by "."
/// and "..": an entry the folders share by name, compared without case, takes
/// the earliest folder's file and the latest folder's spelling, as copying
/// the folders over each other from the last to the first would leave it.
/// One folder is listed as loose_listing() lists it.
///
/// @param folders the folders, highest precedence first; a missing one adds nothing
/// @param roots the folders are the store's layered folders, whose hidden
///        folder (hidden_at_top) is left out
/// @return the entries; empty when no folder exists
std::vector<LooseItem>
merged_listing(std::span<const std::filesystem::path> folders, bool roots = false) {
    if (folders.size() == 1) {
        auto items = loose_listing(folders.front());
        if (roots)
            std::erase_if(items, [](const LooseItem& item) {
                return hidden_at_top(normalized_path(item.name));
            });
        return items;
    }
    std::unordered_map<std::string, LooseItem> merged;
    bool present = false;
    for (auto folder = folders.rbegin(); folder != folders.rend(); ++folder) {
        auto items = loose_listing(*folder);
        if (items.empty())
            continue;
        present = true;
        for (auto& item : items) {
            if (dot_entry(item) || (roots && hidden_at_top(normalized_path(item.name))))
                continue;
            auto [slot, inserted] = merged.try_emplace(normalized_path(item.name), item);
            if (!inserted) {
                slot->second.path = std::move(item.path);
                slot->second.directory = item.directory;
                slot->second.size = item.size;
            }
        }
    }
    std::vector<LooseItem> items;
    if (!present)
        return items;
    items.reserve(merged.size() + 2);
    for (auto& [key, item] : merged)
        items.push_back(std::move(item));
    std::sort(items.begin(), items.end(), [](const LooseItem& a, const LooseItem& b) {
        return ntfs_less(a.name, b.name);
    });
    const auto& first = folders.front();
    items.insert(items.begin(), LooseItem{std::string(kParentDirectory), first / "..", true, 0});
    items.insert(items.begin(), LooseItem{std::string(kCurrentDirectory), first, true, 0});
    return items;
}

/// Finds a folder below a root by a folded relative path, each part matched without case.
///
/// @param root the folder searched from
/// @param key '/'-separated path, ASCII lower case; empty for the root itself
/// @return the host folder, or nullopt when a part is missing or not a folder
std::optional<std::filesystem::path>
find_loose_directory(const std::filesystem::path& root, const std::string& key) {
    if (hidden_at_top(first_part(key)))
        return std::nullopt;
    auto candidate = root;
    for (std::size_t begin = 0; begin < key.size();) {
        const auto end = key.find('/', begin);
        const auto part = key.substr(begin, end == std::string::npos ? end : end - begin);
        bool present = false;
        for (const auto& item : loose_listing(candidate))
            if (item.directory && !dot_entry(item) && normalized_path(item.name) == part) {
                candidate = item.path;
                present = true;
                break;
            }
        if (!present)
            return std::nullopt;
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    // A folder a link leads to outside the root is not the root's.
    if (!key.empty() && !stays_inside(root, candidate))
        return std::nullopt;
    return candidate;
}

/// Tells whether a folded path is another one or lies below it, whole
/// '/'-separated parts compared.
///
/// @param head the folded path above; empty lies above every path
/// @param path the folded path tested
/// @return true when `path` is `head` or lies below it
bool path_within(std::string_view head, std::string_view path) noexcept {
    return head.empty() || path == head ||
           (path.size() > head.size() && path.starts_with(head) && path[head.size()] == '/');
}

/// Joins two folded paths with '/', leaving out the separator beside an empty one.
///
/// @param head the folded path of a folder
/// @param name the folded name of a record in it
/// @return the folded path of the record
std::string joined_key(std::string_view head, std::string_view name) {
    std::string key;
    key.reserve(head.size() + 1 + name.size());
    key.append(head);
    if (!head.empty() && !name.empty())
        key.push_back('/');
    key.append(name);
    return key;
}

/// Hands on the folded path of every file record of an archive that a
/// listing of a folder could take, in stored depth-first order.
///
/// A record's folded path is its '/'-joined path through the archive,
/// folded as normalized_path() folds it, so every record entries() lists
/// under the folder's path is handed on, duplicates and records a lookup
/// cannot reach among them. Subtrees whose records cannot lie in the folder,
/// or, unless `recursive`, deeper than its own files, are not walked.
///
/// @param archive the archive
/// @param folder folded path of the folder, '/'-separated; empty for the root
/// @param recursive true when the listing takes files below the folder's subfolders too
/// @param take called with each folded path
template <class Take>
void archive_listing(
    const HpiArchive& archive, std::string_view folder, bool recursive, Take&& take
) {
    const auto nodes = archive.nodes();
    // Whether a folder of this folded path can hold records the listing takes.
    const auto worth_walking = [&](std::string_view key) {
        if (path_within(key, folder))
            return true;
        if (!path_within(folder, key))
            return false;
        if (recursive || key == folder)
            return true;
        const auto rest = folder.empty() ? key : key.substr(folder.size() + 1);
        return rest.find('/') == std::string_view::npos;
    };
    const auto visit = [&](const auto& self, uint32_t directory, const std::string& key) -> void {
        const ArchiveNode& parent = nodes[directory];
        for (uint32_t i = 0; i < parent.child_count; ++i) {
            const auto index = parent.first_child + i;
            const ArchiveNode& node = nodes[index];
            std::string child = joined_key(key, normalized_path(node.name));
            if (!node.directory())
                take(std::move(child));
            else if (worth_walking(child))
                self(self, index, child);
        }
    };
    visit(visit, 0, std::string());
}

} // namespace

// One published pack layer. It is never changed after the store shares it:
// a new set of hidden marks is a new PackLayer that shares the zip source.
struct AssetStore::PackLayer {
    // The open zip, shared by every copy of the layer. The lock is held only
    // around one read of the file, never across inflation.
    struct ZipSource {
        base::threads::Mutex lock;
        std::ifstream file;
        uint64_t size = 0;
        formats::zip::StreamDirectory directory;
    };

    struct Entry {
        std::string path;
        std::string key;
        std::string name;
        std::filesystem::path host;
        std::size_t zip_index = 0;
        uint32_t size = 0;
        bool hidden = false;
        // The bytes the layer was given for this file; null reads the pack.
        std::shared_ptr<const std::vector<uint8_t>> contents;
    };

    // One child of a folder the layer's paths imply. A directory child is the
    // first spelling of that prefix; a file child points at entries.
    struct Child {
        std::string name;
        bool directory = false;
        uint32_t size = 0;
        std::size_t entry = 0;
    };

    struct Folder {
        std::vector<Child> children;
    };

    PackLayerKind kind = PackLayerKind::folder;
    std::filesystem::path location;
    std::string label;
    uint64_t serial = 0;
    std::vector<Entry> entries;
    std::unordered_map<std::string, std::size_t> index;
    std::unordered_map<std::string, Folder> folders;
    std::shared_ptr<ZipSource> zip;
};

struct AssetStore::PackLayerSlot {
    base::threads::Mutex lock;
    std::shared_ptr<const PackLayer> layer;
};

// Published once and never changed. A lookup holds a shared_ptr to one copy.
struct AssetStore::MutatorLayer {
    struct Entry {
        std::string path;
        std::string key;
        std::string name;
        std::string owner;
        MutatorSourceKind kind = MutatorSourceKind::hidden;
        std::filesystem::path host;
        std::size_t archive = 0;
        std::string archive_path;
        uint32_t node = 0;
        uint32_t size = 0;
        std::shared_ptr<const std::vector<uint8_t>> contents;
    };

    // One child of a folder the served paths imply. A directory child is the
    // first spelling of that prefix; a file child points at entries. Hidden
    // paths are indexed and do not appear here.
    struct Child {
        std::string name;
        bool directory = false;
        uint32_t size = 0;
        std::size_t entry = 0;
    };

    struct Folder {
        std::vector<Child> children;
    };

    struct Mask {
        std::string pattern;
        std::string owner;
    };

    struct Archive {
        std::shared_ptr<const HpiArchive> archive;
        std::filesystem::path location;
        std::string owner;
    };

    std::string label;
    uint64_t serial = 0;
    std::vector<Entry> entries;
    std::unordered_map<std::string, std::size_t> index;
    std::unordered_map<std::string, Folder> folders;
    std::vector<Mask> masks;
    std::vector<Archive> archives;
};

struct AssetStore::MutatorLayerSlot {
    base::threads::Mutex lock;
    std::shared_ptr<const MutatorLayer> layer;
};

namespace {

/// Returns the next serial a pack layer is published with.
///
/// @return the serial, different for every call in this process
uint64_t next_pack_serial() {
    static std::atomic<uint64_t> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

/// Returns why a pack path is one a loose lookup would refuse, or empty.
///
/// @param path the shown path or the source path
/// @return the reason, or empty when the path can be stored
std::string pack_path_problem(std::string_view path) {
    if (path.empty())
        return "empty path";
    if (path.front() == '/' || path.front() == '\\')
        return "absolute path";
    if (path.find(':') != std::string_view::npos)
        return "drive-qualified path";
    if (path.find('\0') != std::string_view::npos)
        return "path contains NUL";
    if (path.size() > kMaxPathLength)
        return "path is too long";
    const auto key = normalized_path(path);
    if (key.empty())
        return "empty path";
    if (hidden_at_top(first_part(key)))
        return "path starts with the backup folder";
    for (std::size_t begin = 0; begin < key.size();) {
        const auto end = key.find('/', begin);
        const auto part = key.substr(begin, end == std::string::npos ? end : end - begin);
        if (part.empty() || part == "." || part == "..")
            return "path contains traversal";
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    return {};
}

/// Sets a refusal that names one pack file.
///
/// @param error receives the message
/// @param file the path at fault
/// @param reason why it was refused
void pack_file_error(std::string& error, std::string_view file, std::string_view reason) {
    error = "pack layer file '" + std::string(file) + "': " + std::string(reason);
}

/// Splits a pack path into its parts, keeping each part's spelling.
///
/// @param path the path, with '/' or '\\' between parts
/// @return the parts, empty segments dropped
std::vector<std::string> pack_parts(std::string_view path) {
    std::vector<std::string> parts;
    std::string current;
    for (const char character : path) {
        if (character == '/' || character == '\\') {
            if (!current.empty())
                parts.push_back(std::move(current));
            current.clear();
            continue;
        }
        current.push_back(character);
    }
    if (!current.empty())
        parts.push_back(std::move(current));
    return parts;
}

/// Resolves one source file below a pack folder, ignoring ASCII case.
///
/// As loose_path_listed_in: two names that fold alike are refused, the file
/// must be regular, and a link that leaves the folder is refused.
///
/// @param root the pack folder
/// @param source the source path
/// @param[out] reason why the file cannot be taken
/// @return the host file, or nullopt
std::optional<std::filesystem::path>
resolve_pack_file(const std::filesystem::path& root, std::string_view source, std::string& reason) {
    const auto key = normalized_path(source);
    auto current = root;
    for (std::size_t begin = 0; begin < key.size();) {
        const auto end = key.find('/', begin);
        const auto part = key.substr(begin, end == std::string::npos ? end : end - begin);
        std::error_code error;
        if (!std::filesystem::is_directory(current, error)) {
            reason = "not a file in the pack";
            return std::nullopt;
        }
        std::optional<std::filesystem::path> match;
        for (const auto& item : std::filesystem::directory_iterator(current, error)) {
            if (normalized_path(utf8_text(item.path().filename())) != part)
                continue;
            if (match) {
                reason = "ambiguous case";
                return std::nullopt;
            }
            match = item.path();
        }
        if (error || !match) {
            reason = "not a file in the pack";
            return std::nullopt;
        }
        current = *match;
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    if (!stays_inside(root, current)) {
        reason = "leaves the pack";
        return std::nullopt;
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(current, error)) {
        reason = "not a regular file";
        return std::nullopt;
    }
    const auto size = std::filesystem::file_size(current, error);
    if (error || !formats::hpi::entry_size_allowed(size)) {
        reason = "exceeds the entry size limit";
        return std::nullopt;
    }
    return current;
}

} // namespace

// Builds and reads a pack layer. A friend of AssetStore so it can name the
// private layer type from code that is not a member.
struct PackLayerOps {
    static bool zip_read_at(void* context, uint64_t offset, std::span<uint8_t> bytes);
    static std::vector<uint8_t>
    read_pack_zip(const AssetStore::PackLayer& layer, const AssetStore::PackLayer::Entry& entry);
    static AssetData
    read_pack_entry(const AssetStore::PackLayer& layer, const AssetStore::PackLayer::Entry& entry);
    static bool add_pack_entry(
        AssetStore::PackLayer& layer,
        const std::string& path,
        const std::filesystem::path& host,
        std::size_t zip_index,
        uint32_t size,
        std::string& error
    );
    static bool
    add_given_entry(AssetStore::PackLayer& layer, const PackLayerFile& file, std::string& error);
    static std::shared_ptr<AssetStore::PackLayer>
    make_pack_layer(const PackLayerSpec& spec, std::string& error);
    static void append_pack_matches(
        std::vector<FoundEntry>& found,
        const AssetStore::PackLayer& layer,
        std::string_view directory,
        std::string_view spec,
        int mount
    );
};

/// Reads one range of a pack zip. The lock is released before the caller inflates.
///
/// @param context the ZipSource
/// @param offset file offset of the first byte
/// @param bytes receives bytes.size() bytes
/// @return true when every byte was read
bool PackLayerOps::zip_read_at(void* context, uint64_t offset, std::span<uint8_t> bytes) {
    auto* source = static_cast<AssetStore::PackLayer::ZipSource*>(context);
    const base::threads::LockGuard guard(source->lock);
    if (offset > source->size || bytes.size() > source->size - offset)
        return false;
    source->file.clear();
    source->file.seekg(static_cast<std::streamoff>(offset));
    if (!source->file)
        return false;
    source->file.read(
        reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
    );
    return static_cast<std::size_t>(source->file.gcount()) == bytes.size();
}

/// Reads one zip entry of a pack layer.
///
/// @param layer the layer
/// @param entry the layer's file
/// @return the entry's bytes
/// @throws std::runtime_error naming the path, the label and the reason
std::vector<uint8_t> PackLayerOps::read_pack_zip(
    const AssetStore::PackLayer& layer, const AssetStore::PackLayer::Entry& entry
) {
    formats::zip::SourceHooks hooks{layer.zip.get(), zip_read_at};
    std::vector<uint8_t> bytes;
    formats::zip::ZipError error;
    const auto& zip_entry = layer.zip->directory.entries[entry.zip_index];
    if (!formats::zip::read_stream_entry(
            hooks, layer.zip->size, zip_entry, formats::hpi::EntryByteLimit, bytes, error
        )) {
        fail(
            "pack layer '" + layer.label + "' cannot read '" + entry.path +
            "': " + formats::zip::zip_status_message(error.status)
        );
    }
    return bytes;
}

/// Reads a pack layer's own copy of one of its files.
///
/// @param layer the layer
/// @param entry the layer's file
/// @return the bytes, the providing path and whether they came from a zip
AssetData PackLayerOps::read_pack_entry(
    const AssetStore::PackLayer& layer, const AssetStore::PackLayer::Entry& entry
) {
    if (entry.contents)
        return {*entry.contents, layer.location, layer.kind == PackLayerKind::zip};
    if (layer.kind == PackLayerKind::zip)
        return {read_pack_zip(layer, entry), layer.location, true};
    try {
        return {read_loose(entry.host), entry.host, false};
    } catch (const std::runtime_error& error) {
        fail("pack layer '" + layer.label + "' cannot read '" + entry.path + "': " + error.what());
    }
}

/// Adds one shown file and the folder prefixes it implies.
///
/// @param layer the layer being built
/// @param path the path the store shows
/// @param host the resolved folder file; empty for a zip
/// @param zip_index the zip entry, when host is empty
/// @param size the file's size
/// @param[out] error the reason when the path collides with a folder
/// @return false when the path cannot be added
bool PackLayerOps::add_pack_entry(
    AssetStore::PackLayer& layer,
    const std::string& path,
    const std::filesystem::path& host,
    std::size_t zip_index,
    uint32_t size,
    std::string& error
) {
    const auto parts = pack_parts(path);
    std::string prefix;
    for (std::size_t index = 0; index + 1 < parts.size(); ++index) {
        const auto folded = normalized_path(parts[index]);
        const std::string next = prefix.empty() ? folded : prefix + "/" + folded;
        if (layer.index.contains(next)) {
            pack_file_error(error, path, "'" + parts[index] + "' is a file");
            return false;
        }
        auto& folder = layer.folders[prefix];
        bool present = false;
        for (const auto& child : folder.children) {
            if (child.directory && normalized_path(child.name) == folded) {
                present = true;
                break;
            }
        }
        if (!present)
            folder.children.push_back({parts[index], true, 0, 0});
        prefix = next;
    }
    const auto key = normalized_path(path);
    if (layer.folders.contains(key)) {
        pack_file_error(error, path, "the path is a folder");
        return false;
    }
    AssetStore::PackLayer::Entry stored;
    stored.path = path;
    stored.key = key;
    stored.name = parts.empty() ? path : parts.back();
    stored.host = host;
    stored.zip_index = zip_index;
    stored.size = size;
    const auto index = layer.entries.size();
    layer.entries.push_back(std::move(stored));
    layer.index.emplace(key, index);
    layer.folders[prefix].children.push_back({layer.entries.back().name, false, size, index});
    return true;
}

/// Adds one file the layer was given as its contents.
///
/// @param layer the layer being built
/// @param file the file; its contents are not null
/// @param[out] error the reason when the file cannot be added
/// @return false when the contents are past the entry size limit or the path
///         collides with a folder
bool PackLayerOps::add_given_entry(
    AssetStore::PackLayer& layer, const PackLayerFile& file, std::string& error
) {
    if (!formats::hpi::entry_size_allowed(file.contents->size())) {
        pack_file_error(error, file.path, "exceeds the entry size limit");
        return false;
    }
    if (!add_pack_entry(
            layer, file.path, {}, 0, static_cast<uint32_t>(file.contents->size()), error
        ))
        return false;
    layer.entries.back().contents = file.contents;
    return true;
}

/// Builds a pack layer without publishing it.
///
/// The count, the label and the paths are checked before any file is opened.
///
/// @param spec the folder or zip, the files it shows and its label
/// @param[out] error the reason when no layer is returned
/// @return the layer, or null
std::shared_ptr<AssetStore::PackLayer>
PackLayerOps::make_pack_layer(const PackLayerSpec& spec, std::string& error) {
    if (spec.files.empty()) {
        error = "a pack layer lists no files";
        return nullptr;
    }
    if (spec.files.size() > formats::hpi::PackLayerFileLimit) {
        pack_file_error(
            error,
            spec.files[formats::hpi::PackLayerFileLimit].path,
            "a pack layer lists at most " + std::to_string(formats::hpi::PackLayerFileLimit) +
                " files"
        );
        return nullptr;
    }
    if (spec.label.empty()) {
        error = "a pack layer needs a label";
        return nullptr;
    }
    if (spec.label.find('\n') != std::string::npos || spec.label.find('\r') != std::string::npos) {
        error = "a pack layer label holds more than one line";
        return nullptr;
    }
    std::unordered_map<std::string, std::string> seen;
    for (const auto& file : spec.files) {
        if (const auto problem = pack_path_problem(file.path); !problem.empty()) {
            pack_file_error(error, file.path, problem);
            return nullptr;
        }
        if (const auto problem = file.contents ? std::string() : pack_path_problem(file.source);
            !problem.empty()) {
            pack_file_error(error, file.source, problem);
            return nullptr;
        }
        const auto key = normalized_path(file.path);
        const auto [slot, inserted] = seen.emplace(key, file.path);
        if (!inserted) {
            pack_file_error(error, file.path, "the same path as " + slot->second);
            return nullptr;
        }
    }

    auto layer = std::make_shared<AssetStore::PackLayer>();
    layer->kind = spec.kind;
    layer->label = spec.label;
    layer->location = resolved(spec.location);

    if (spec.kind == PackLayerKind::folder) {
        std::error_code status;
        if (!std::filesystem::is_directory(layer->location, status)) {
            pack_file_error(error, utf8_text(spec.location), "not a folder");
            return nullptr;
        }
        for (const auto& file : spec.files) {
            if (file.contents) {
                if (!add_given_entry(*layer, file, error))
                    return nullptr;
                continue;
            }
            std::string reason;
            const auto host = resolve_pack_file(layer->location, file.source, reason);
            if (!host) {
                pack_file_error(error, file.source, reason);
                return nullptr;
            }
            std::error_code size_error;
            const auto size = std::filesystem::file_size(*host, size_error);
            if (size_error || !formats::hpi::entry_size_allowed(size)) {
                pack_file_error(error, file.source, "exceeds the entry size limit");
                return nullptr;
            }
            if (!add_pack_entry(*layer, file.path, *host, 0, static_cast<uint32_t>(size), error))
                return nullptr;
        }
        return layer;
    }

    auto zip = std::make_shared<AssetStore::PackLayer::ZipSource>();
    std::error_code status;
    zip->size = std::filesystem::file_size(layer->location, status);
    zip->file.open(layer->location, std::ios::binary);
    if (status || !zip->file) {
        pack_file_error(error, utf8_text(spec.location), "cannot open the zip");
        return nullptr;
    }
    formats::zip::SourceHooks hooks{zip.get(), zip_read_at};
    formats::zip::ZipError zip_error;
    if (!formats::zip::read_stream_directory(
            hooks, zip->size, formats::zip::StreamLimits{}, zip->directory, zip_error
        )) {
        pack_file_error(
            error, utf8_text(spec.location), formats::zip::zip_status_message(zip_error.status)
        );
        return nullptr;
    }
    layer->zip = std::move(zip);
    for (const auto& file : spec.files) {
        if (file.contents) {
            if (!add_given_entry(*layer, file, error))
                return nullptr;
            continue;
        }
        const auto key = normalized_path(file.source);
        std::vector<std::size_t> matches;
        for (std::size_t index = 0; index < layer->zip->directory.entries.size(); ++index)
            if (normalized_path(layer->zip->directory.entries[index].name) == key)
                matches.push_back(index);
        if (matches.size() != 1) {
            pack_file_error(
                error,
                file.source,
                matches.empty() ? "not a file in the pack" : "names more than one entry"
            );
            return nullptr;
        }
        const auto& zip_entry = layer->zip->directory.entries[matches[0]];
        if (zip_entry.directory || zip_entry.symbolic_link || zip_entry.special_file) {
            pack_file_error(error, file.source, "not a file in the pack");
            return nullptr;
        }
        if (!formats::hpi::entry_size_allowed(zip_entry.bytes)) {
            pack_file_error(error, file.source, "exceeds the entry size limit");
            return nullptr;
        }
        if (!add_pack_entry(
                *layer, file.path, {}, matches[0], static_cast<uint32_t>(zip_entry.bytes), error
            ))
            return nullptr;
    }
    return layer;
}

/// Appends a pack folder's children that match a find.
///
/// @param[in,out] found matches appended in the layer's order
/// @param layer the layer
/// @param directory directory part of the pattern, with its trailing separator
/// @param spec wildcard for the names
/// @param mount mount index past every archive
void PackLayerOps::append_pack_matches(
    std::vector<FoundEntry>& found,
    const AssetStore::PackLayer& layer,
    std::string_view directory,
    std::string_view spec,
    int mount
) {
    std::string folder;
    if (!directory.empty()) {
        if (directory.back() == '/' || directory.back() == '\\')
            directory.remove_suffix(1);
        folder = normalized_path(directory);
    }
    const auto listed = layer.folders.find(folder);
    if (listed == layer.folders.end())
        return;
    for (const auto& child : listed->second.children) {
        if (!match_wildcard(child.name, spec))
            continue;
        if (!child.directory && layer.entries[child.entry].hidden)
            continue;
        found.push_back(
            {child.name, child.directory, child.directory ? 0 : child.size, mount, true}
        );
    }
}

namespace {

/// Returns why a mutator's id is refused, or null when it can be stored.
///
/// @param owner the mutator's id
/// @return a static reason, or null
const char* owner_problem(std::string_view owner) noexcept {
    if (owner.empty())
        return "needs an owner";
    if (owner.find('\n') != std::string_view::npos || owner.find('\r') != std::string_view::npos)
        return "owner holds a line break";
    return nullptr;
}

/// Returns whether a loose lookup would throw for this path.
///
/// The mutator layer does not answer those paths, so the loose lookup still
/// reports them. A backup-folder path does not throw; a mask may hide it.
///
/// @param resource the path the caller wrote
/// @return true when loose_path would throw
bool lookup_path_throws(std::string_view resource) {
    if (resource.empty() || resource.front() == '/' || resource.front() == '\\' ||
        resource.find(':') != std::string_view::npos ||
        resource.find('\0') != std::string_view::npos || resource.size() > kMaxPathLength)
        return true;
    const auto key = normalized_path(resource);
    for (std::size_t begin = 0; begin < key.size();) {
        const auto end = key.find('/', begin);
        const auto part = key.substr(begin, end == std::string::npos ? end : end - begin);
        if (part == "." || part == "..")
            return true;
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    return false;
}

/// Joins a find's directory to a child name.
///
/// @param directory directory part of the pattern, with its trailing separator
/// @param name the child name
/// @return the resource path
std::string child_path(std::string_view directory, std::string_view name) {
    if (directory.empty())
        return std::string(name);
    std::string path;
    path.reserve(directory.size() + name.size());
    path.assign(directory);
    path.append(name);
    return path;
}

/// Tells the observer that the mutator layer served or hid a path.
///
/// @param observer the store's observer
/// @param path the path, as the lookup should report it
/// @param owner the mutator
/// @param hidden true when the layer hid the path
void note_mutator(
    const LookupObserver& observer, std::string_view path, std::string_view owner, bool hidden
) {
    if (observer.mutator_layer_used != nullptr)
        observer.mutator_layer_used(observer.context, path, owner, hidden);
}

/// Sets a refusal that names one mutator-layer path.
///
/// @param error receives the message
/// @param path the path at fault
/// @param owner the mutator
/// @param reason why it was refused
void mutator_entry_error(
    std::string& error, std::string_view path, std::string_view owner, std::string_view reason
) {
    error = "mutator layer file '" + std::string(path) + "' of '" + std::string(owner) +
            "': " + std::string(reason);
}

/// Sets a refusal that names one mask.
///
/// @param error receives the message
/// @param pattern the pattern at fault
/// @param owner the mutator
/// @param reason why it was refused
void mutator_mask_error(
    std::string& error, std::string_view pattern, std::string_view owner, std::string_view reason
) {
    error = "mutator layer mask '" + std::string(pattern) + "' of '" + std::string(owner) +
            "': " + std::string(reason);
}

/// Sets a refusal that names one archive's mutator.
///
/// @param error receives the message
/// @param owner the mutator
/// @param reason why it was refused
void mutator_archive_error(std::string& error, std::string_view owner, std::string_view reason) {
    error = "mutator layer archive of '" + std::string(owner) + "': " + std::string(reason);
}

} // namespace

// Builds and reads a mutator layer. A friend of AssetStore so it can name
// the private layer type from code that is not a member.
struct MutatorLayerOps {
    struct Decision {
        const AssetStore::MutatorLayer::Entry* entry = nullptr;
        const AssetStore::MutatorLayer::Mask* mask = nullptr;

        /// Returns whether the layer serves or hides the path.
        ///
        /// @return true for a table entry or a matching mask
        [[nodiscard]] bool answers() const { return entry != nullptr || mask != nullptr; }

        /// Returns whether the path is missing.
        ///
        /// @return true for a hidden entry or a matching mask
        [[nodiscard]] bool hidden() const {
            return mask != nullptr ||
                   (entry != nullptr && entry->kind == MutatorSourceKind::hidden);
        }

        /// Returns the mutator that served or hid the path.
        ///
        /// @return the owner, or empty when the layer does not answer
        [[nodiscard]] std::string_view owner() const {
            if (entry != nullptr)
                return entry->owner;
            if (mask != nullptr)
                return mask->owner;
            return {};
        }
    };

    struct Hit {
        std::shared_ptr<const AssetStore::MutatorLayer> layer;
        Decision decision;
    };

    static Decision decide(const AssetStore::MutatorLayer& layer, std::string_view path);
    static Hit consult(const AssetStore& store, std::string_view resource);
    static bool omit_file(
        const AssetStore::MutatorLayer* layer,
        std::string_view path,
        const LookupObserver& observer,
        std::unordered_set<std::string>& noted
    );
    static AssetData
    read_entry(const AssetStore::MutatorLayer& layer, const AssetStore::MutatorLayer::Entry& entry);
    static Provider provider_of(
        const AssetStore::MutatorLayer& layer, const AssetStore::MutatorLayer::Entry& entry
    );
    static ResourceFile* open_entry(
        const std::shared_ptr<const AssetStore::MutatorLayer>& layer,
        const AssetStore::MutatorLayer::Entry& entry
    );
    static bool add_served(
        AssetStore::MutatorLayer& layer,
        const MutatorLayerEntry& entry,
        uint32_t size,
        uint32_t node,
        std::string& error
    );
    static std::shared_ptr<AssetStore::MutatorLayer>
    make(const MutatorLayerSpec& spec, std::string& error);
    static void append_matches(
        std::vector<FoundEntry>& found,
        const AssetStore::MutatorLayer& layer,
        std::string_view directory,
        std::string_view spec
    );
};

/// Returns the layer's answer for one path: a table entry, else the first mask.
///
/// A table entry wins over every mask. The path is folded, so case and slash
/// do not matter.
///
/// @param layer the layer
/// @param path the resource path
/// @return the entry or the mask, or neither
MutatorLayerOps::Decision
MutatorLayerOps::decide(const AssetStore::MutatorLayer& layer, std::string_view path) {
    Decision decision;
    const auto key = normalized_path(path);
    if (const auto found = layer.index.find(key); found != layer.index.end()) {
        decision.entry = &layer.entries[found->second];
        return decision;
    }
    for (const auto& mask : layer.masks) {
        if (mutator_mask_matches(mask.pattern, path)) {
            decision.mask = &mask;
            return decision;
        }
    }
    return decision;
}

/// Copies the mounted layer once and decides one path against that copy.
///
/// A path a loose lookup would throw for is left undecided. No layer is
/// copied when none is mounted.
///
/// @param store the store
/// @param resource the path the caller wrote
/// @return the copy and the decision
MutatorLayerOps::Hit MutatorLayerOps::consult(const AssetStore& store, std::string_view resource) {
    Hit hit;
    if (lookup_path_throws(resource))
        return hit;
    hit.layer = store.mutator_layer_snapshot();
    if (!hit.layer)
        return hit;
    hit.decision = decide(*hit.layer, resource);
    return hit;
}

/// Returns whether a file is left out because the layer serves or hides it.
///
/// Directories are not passed here. A hidden path is reported once.
///
/// @param layer the layer copied for this lookup, or null
/// @param path the file's resource path
/// @param observer the store's observer
/// @param[in,out] noted hidden paths already reported
/// @return true when the file is left out
bool MutatorLayerOps::omit_file(
    const AssetStore::MutatorLayer* layer,
    std::string_view path,
    const LookupObserver& observer,
    std::unordered_set<std::string>& noted
) {
    if (layer == nullptr)
        return false;
    const auto decision = decide(*layer, path);
    if (!decision.answers())
        return false;
    if (decision.hidden()) {
        const auto key = normalized_path(path);
        if (noted.insert(key).second)
            note_mutator(observer, key, decision.owner(), true);
    }
    return true;
}

/// Reads one served path. The caller does not hold the layer's lock.
///
/// @param layer the layer
/// @param entry the layer's file; not hidden
/// @return the bytes, the providing path and whether they count as archived
/// @throws std::runtime_error naming the path, the label and the reason
AssetData MutatorLayerOps::read_entry(
    const AssetStore::MutatorLayer& layer, const AssetStore::MutatorLayer::Entry& entry
) {
    if (entry.kind == MutatorSourceKind::contents)
        return {*entry.contents, {}, true};
    if (entry.kind == MutatorSourceKind::loose) {
        try {
            return {read_loose(entry.host), entry.host, false};
        } catch (const std::runtime_error& error) {
            fail(
                "mutator layer '" + layer.label + "' cannot read '" + entry.path +
                "': " + error.what()
            );
        }
    }
    const auto& held = layer.archives[entry.archive];
    auto bytes = held.archive->read_node(entry.node);
    if (!bytes.ok())
        fail(
            "mutator layer '" + layer.label + "' cannot read '" + entry.path +
            "': " + bytes.error.message
        );
    return {std::move(*bytes.value), held.location, true};
}

/// Returns the provider of one served path.
///
/// @param layer the layer
/// @param entry the layer's file; not hidden
/// @return kind mutator_layer, the host or archive location, and the identity
Provider MutatorLayerOps::provider_of(
    const AssetStore::MutatorLayer& layer, const AssetStore::MutatorLayer::Entry& entry
) {
    std::filesystem::path path;
    if (entry.kind == MutatorSourceKind::loose)
        path = entry.host;
    else if (entry.kind == MutatorSourceKind::archive)
        path = layer.archives[entry.archive].location;
    return {
        ProviderKind::mutator_layer,
        std::move(path),
        "mutator:" + entry.owner + "#" + std::to_string(layer.serial)
    };
}

/// Adds one served file and the folder prefixes it implies.
///
/// A hidden entry already stored at a prefix is not a file, so it does not
/// collide with a served path below it. A served file does.
///
/// @param layer the layer being built
/// @param entry the spec entry
/// @param size the file's size
/// @param node the archive node, when the entry is an archive file
/// @param[out] error the reason when the path collides with a served file or folder
/// @return false when the path cannot be added
bool MutatorLayerOps::add_served(
    AssetStore::MutatorLayer& layer,
    const MutatorLayerEntry& entry,
    uint32_t size,
    uint32_t node,
    std::string& error
) {
    const auto parts = pack_parts(entry.path);
    std::string prefix;
    for (std::size_t part_index = 0; part_index + 1 < parts.size(); ++part_index) {
        const auto folded = normalized_path(parts[part_index]);
        const std::string next = prefix.empty() ? folded : prefix + "/" + folded;
        if (const auto prior = layer.index.find(next); prior != layer.index.end()) {
            if (layer.entries[prior->second].kind != MutatorSourceKind::hidden) {
                mutator_entry_error(
                    error, entry.path, entry.owner, "'" + parts[part_index] + "' is a file"
                );
                return false;
            }
        }
        auto& folder = layer.folders[prefix];
        bool present = false;
        for (const auto& child : folder.children) {
            if (child.directory && normalized_path(child.name) == folded) {
                present = true;
                break;
            }
        }
        if (!present)
            folder.children.push_back({parts[part_index], true, 0, 0});
        prefix = next;
    }
    const auto key = normalized_path(entry.path);
    if (layer.folders.contains(key)) {
        mutator_entry_error(error, entry.path, entry.owner, "the path is a folder");
        return false;
    }
    AssetStore::MutatorLayer::Entry stored;
    stored.path = entry.path;
    stored.key = key;
    stored.name = parts.empty() ? entry.path : parts.back();
    stored.owner = entry.owner;
    stored.kind = entry.kind;
    stored.host = entry.host;
    stored.archive = entry.archive;
    stored.archive_path = entry.archive_path;
    stored.node = node;
    stored.size = size;
    stored.contents = entry.contents;
    const auto stored_index = layer.entries.size();
    layer.entries.push_back(std::move(stored));
    layer.index.emplace(key, stored_index);
    layer.folders[prefix].children.push_back(
        {layer.entries.back().name, false, size, stored_index}
    );
    return true;
}

/// Builds a mutator layer without publishing it.
///
/// The counts, the label, the owners, the paths and the masks are checked
/// before any file is opened. A refusal returns null and publishes nothing.
///
/// @param spec the archives, paths, masks and label
/// @param[out] error the reason when no layer is returned
/// @return the layer, or null
std::shared_ptr<AssetStore::MutatorLayer>
MutatorLayerOps::make(const MutatorLayerSpec& spec, std::string& error) {
    if (spec.entries.size() > formats::hpi::MutatorLayerEntryLimit) {
        const auto& entry = spec.entries[formats::hpi::MutatorLayerEntryLimit];
        mutator_entry_error(
            error,
            entry.path,
            entry.owner,
            "a mutator layer lists at most " +
                std::to_string(formats::hpi::MutatorLayerEntryLimit) + " paths"
        );
        return nullptr;
    }
    if (spec.masks.size() > formats::hpi::MutatorLayerPatternLimit) {
        const auto& mask = spec.masks[formats::hpi::MutatorLayerPatternLimit];
        mutator_mask_error(
            error,
            mask.pattern,
            mask.owner,
            "a mutator layer lists at most " +
                std::to_string(formats::hpi::MutatorLayerPatternLimit) + " masks"
        );
        return nullptr;
    }
    if (spec.archives.size() > formats::hpi::MutatorLayerArchiveLimit) {
        const auto& archive = spec.archives[formats::hpi::MutatorLayerArchiveLimit];
        mutator_archive_error(
            error,
            archive.owner,
            "a mutator layer holds at most " +
                std::to_string(formats::hpi::MutatorLayerArchiveLimit) + " archives"
        );
        return nullptr;
    }
    if (spec.label.empty()) {
        error = "a mutator layer needs a label";
        return nullptr;
    }
    if (spec.label.find('\n') != std::string::npos || spec.label.find('\r') != std::string::npos) {
        error = "a mutator layer label holds a line break";
        return nullptr;
    }
    for (const auto& archive : spec.archives) {
        if (const char* problem = owner_problem(archive.owner)) {
            mutator_archive_error(error, archive.owner, problem);
            return nullptr;
        }
        if (!archive.archive) {
            mutator_archive_error(error, archive.owner, "has no archive");
            return nullptr;
        }
    }
    std::unordered_map<std::string, std::string> seen;
    for (const auto& entry : spec.entries) {
        if (const char* problem = owner_problem(entry.owner)) {
            mutator_entry_error(error, entry.path, entry.owner, problem);
            return nullptr;
        }
        if (const auto problem = pack_path_problem(entry.path); !problem.empty()) {
            mutator_entry_error(error, entry.path, entry.owner, problem);
            return nullptr;
        }
        const auto key = normalized_path(entry.path);
        const auto [slot, inserted] = seen.emplace(key, entry.path);
        if (!inserted) {
            mutator_entry_error(error, entry.path, entry.owner, "the same path as " + slot->second);
            return nullptr;
        }
        if (entry.kind == MutatorSourceKind::archive && entry.archive >= spec.archives.size()) {
            mutator_entry_error(error, entry.path, entry.owner, "archive index is out of range");
            return nullptr;
        }
        if (entry.kind == MutatorSourceKind::contents && !entry.contents) {
            mutator_entry_error(error, entry.path, entry.owner, "has no contents");
            return nullptr;
        }
    }
    for (const auto& mask : spec.masks) {
        if (const char* problem = owner_problem(mask.owner)) {
            mutator_mask_error(error, mask.pattern, mask.owner, problem);
            return nullptr;
        }
        if (const auto problem = mutator_mask_problem(mask.pattern); !problem.empty()) {
            mutator_mask_error(error, mask.pattern, mask.owner, problem);
            return nullptr;
        }
    }

    auto layer = std::make_shared<AssetStore::MutatorLayer>();
    layer->label = spec.label;
    layer->archives.reserve(spec.archives.size());
    for (const auto& archive : spec.archives)
        layer->archives.push_back({archive.archive, archive.location, archive.owner});
    layer->masks.reserve(spec.masks.size());
    for (const auto& mask : spec.masks)
        layer->masks.push_back({mask.pattern, mask.owner});

    uint64_t content_bytes = 0;
    for (const auto& entry : spec.entries) {
        if (entry.kind == MutatorSourceKind::hidden) {
            AssetStore::MutatorLayer::Entry stored;
            stored.path = entry.path;
            stored.key = normalized_path(entry.path);
            const auto parts = pack_parts(entry.path);
            stored.name = parts.empty() ? entry.path : parts.back();
            stored.owner = entry.owner;
            stored.kind = MutatorSourceKind::hidden;
            const auto stored_index = layer->entries.size();
            layer->entries.push_back(std::move(stored));
            layer->index.emplace(layer->entries.back().key, stored_index);
            continue;
        }
        uint32_t size = 0;
        uint32_t node = 0;
        if (entry.kind == MutatorSourceKind::loose) {
            std::error_code status;
            if (!std::filesystem::is_regular_file(entry.host, status) || status) {
                mutator_entry_error(error, entry.path, entry.owner, "not a regular file");
                return nullptr;
            }
            std::error_code size_error;
            const auto bytes = std::filesystem::file_size(entry.host, size_error);
            if (size_error) {
                mutator_entry_error(error, entry.path, entry.owner, "not a regular file");
                return nullptr;
            }
            if (!formats::hpi::entry_size_allowed(bytes)) {
                mutator_entry_error(error, entry.path, entry.owner, "exceeds the entry size limit");
                return nullptr;
            }
            size = static_cast<uint32_t>(bytes);
        } else if (entry.kind == MutatorSourceKind::archive) {
            const auto& held = layer->archives[entry.archive];
            const auto found = held.archive->lookup(entry.archive_path);
            if (!found || held.archive->nodes()[*found].directory()) {
                mutator_entry_error(error, entry.path, entry.owner, "not a file in the archive");
                return nullptr;
            }
            node = *found;
            size = held.archive->nodes()[*found].size;
        } else {
            const auto bytes = static_cast<uint64_t>(entry.contents->size());
            if (!formats::hpi::entry_size_allowed(bytes)) {
                mutator_entry_error(error, entry.path, entry.owner, "exceeds the entry size limit");
                return nullptr;
            }
            if (bytes > formats::hpi::MutatorLayerContentByteLimit ||
                content_bytes > formats::hpi::MutatorLayerContentByteLimit - bytes) {
                mutator_entry_error(
                    error,
                    entry.path,
                    entry.owner,
                    "the given bytes together exceed the content limit"
                );
                return nullptr;
            }
            content_bytes += bytes;
            size = static_cast<uint32_t>(bytes);
        }
        if (!add_served(*layer, entry, size, node, error))
            return nullptr;
    }
    return layer;
}

/// Appends a mutator folder's children that match a find.
///
/// @param[in,out] found matches appended in the layer's order
/// @param layer the layer
/// @param directory directory part of the pattern, with its trailing separator
/// @param spec wildcard for the names
void MutatorLayerOps::append_matches(
    std::vector<FoundEntry>& found,
    const AssetStore::MutatorLayer& layer,
    std::string_view directory,
    std::string_view spec
) {
    std::string folder;
    if (!directory.empty()) {
        if (directory.back() == '/' || directory.back() == '\\')
            directory.remove_suffix(1);
        folder = normalized_path(directory);
    }
    const auto listed = layer.folders.find(folder);
    if (listed == layer.folders.end())
        return;
    for (const auto& child : listed->second.children) {
        if (!match_wildcard(child.name, spec))
            continue;
        found.push_back(
            {child.name,
             child.directory,
             child.directory ? 0 : child.size,
             formats::hpi::MutatorLayerMount,
             false,
             true}
        );
    }
}

struct ResourceFile {
    std::ifstream loose;
    uint32_t loose_size = 0;
    const HpiArchive* archive = nullptr;
    // Host path of the archive, named in the errors of its reads.
    const std::filesystem::path* archive_path = nullptr;
    uint32_t node = 0;
    uint32_t size = 0;
    uint32_t position = 0;
    // Decoded 64 KiB block of a chunked entry, dropped on crossing a block.
    std::vector<uint8_t> block;
    // Where a chunked entry's chunks are stored, read with its first block.
    std::optional<formats::hpi::ChunkTable> chunks;
    // The pack layer this handle was opened from, kept until close.
    std::shared_ptr<const AssetStore::PackLayer> pack_layer;
    // The mutator layer this handle was opened from, kept until close.
    std::shared_ptr<const AssetStore::MutatorLayer> mutator_layer;
    // A zip pack entry, read whole when the handle was opened, or a file the
    // layer was given; in_memory reads it.
    std::vector<uint8_t> memory;
    bool in_memory = false;
    // Given bytes of a mutator layer count as archived. A pack folder's
    // given bytes do not, so this is set only for the mutator layer.
    bool memory_archived = false;
};

/// Opens one served path. The caller does not hold the layer's lock.
///
/// The handle keeps the layer until close, so an archive entry is read from
/// the archive the layer was given.
///
/// @param layer the layer the handle keeps
/// @param entry the layer's file; not hidden
/// @return the handle
/// @throws std::runtime_error when a loose host file cannot be opened
ResourceFile* MutatorLayerOps::open_entry(
    const std::shared_ptr<const AssetStore::MutatorLayer>& layer,
    const AssetStore::MutatorLayer::Entry& entry
) {
    auto* file = new ResourceFile;
    file->mutator_layer = layer;
    if (entry.kind == MutatorSourceKind::contents) {
        try {
            file->memory = *entry.contents;
        } catch (...) {
            delete file;
            throw;
        }
        file->in_memory = true;
        file->memory_archived = true;
        file->size = entry.size;
        return file;
    }
    if (entry.kind == MutatorSourceKind::archive) {
        const auto& held = layer->archives[entry.archive];
        file->archive = held.archive.get();
        file->archive_path = &held.location;
        file->node = entry.node;
        file->size = entry.size;
        return file;
    }
    file->loose.open(entry.host, std::ios::binary);
    if (!file->loose) {
        const auto label = layer->label;
        const auto path = entry.path;
        delete file;
        fail("mutator layer '" + label + "' cannot read '" + path + "': cannot open the file");
    }
    file->loose_size = entry.size;
    file->size = entry.size;
    return file;
}

// Folder listings of the loose tree, keyed by the folded resource path of the
// folder. Each folder is listed once, the first time a lookup passes through it.
struct AssetStore::LooseIndex {
    // The entry a folded name selects in one folder.
    struct Child {
        std::filesystem::path path; // host path of the entry
        bool regular = false;       // a regular file, following links, when listed
        bool directory = false;     // a folder, following links, when listed
        bool ambiguous = false;     // several entries fold to this name
    };

    // Hashes a folded name, so that a part of a key finds it without a copy.
    struct NameHash {
        using is_transparent = void;

        /// Hashes a name.
        ///
        /// @param name the folded name
        /// @return its hash
        std::size_t operator()(std::string_view name) const noexcept {
            return std::hash<std::string_view>{}(name);
        }
    };

    // One folder's listing.
    struct Folder {
        std::size_t entry_count = 0; // entries the listing held
        // Keyed by folded name.
        std::unordered_map<std::string, Child, NameHash, std::equal_to<>> children;
    };

    explicit LooseIndex(std::size_t limit) : entry_limit(limit) {}

    /// Returns a folder's listing, listing the folder the first time.
    ///
    /// Disables the index, dropping every listing, when the listings would
    /// hold more than entry_limit entries.
    ///
    /// @param key folded resource path of the folder, empty for the game directory
    /// @param host host path of the folder
    /// @return the listing, or null when host is not a readable folder or the index was disabled
    const Folder* listing(const std::string& key, const std::filesystem::path& host);
    /// Drops every listing and enables the index again.
    void reset();

    base::threads::Mutex lock;
    const std::size_t entry_limit;
    std::size_t entry_count = 0; // entries held across every listing
    std::atomic<bool> enabled{true};
    std::unordered_map<std::string, Folder> folders;
};

const AssetStore::LooseIndex::Folder*
AssetStore::LooseIndex::listing(const std::string& key, const std::filesystem::path& host) {
    if (const auto kept = folders.find(key); kept != folders.end())
        return &kept->second;
    std::error_code error;
    if (!std::filesystem::is_directory(host, error))
        return nullptr;
    Folder fresh;
    for (const auto& item : std::filesystem::directory_iterator(host, error)) {
        ++fresh.entry_count;
        auto [slot, inserted] =
            fresh.children.try_emplace(normalized_path(utf8_text(item.path().filename())));
        if (!inserted) {
            slot->second.ambiguous = true;
            continue;
        }
        std::error_code status_error;
        slot->second.path = item.path();
        slot->second.regular = item.is_regular_file(status_error);
        slot->second.directory = item.is_directory(status_error);
    }
    if (error)
        return nullptr;
    if (entry_count + fresh.entry_count > entry_limit) {
        folders.clear();
        entry_count = 0;
        enabled.store(false);
        return nullptr;
    }
    entry_count += fresh.entry_count;
    return &(folders[key] = std::move(fresh));
}

void AssetStore::LooseIndex::reset() {
    const base::threads::LockGuard guard(lock);
    folders.clear();
    entry_count = 0;
    enabled.store(true);
}

AssetStore::AssetStore(std::filesystem::path loose_root, std::size_t loose_index_limit)
    : loose_roots_{std::filesystem::absolute(std::move(loose_root))},
      loose_index_(std::make_unique<LooseIndex>(loose_index_limit)),
      pack_layer_(std::make_unique<PackLayerSlot>()),
      mutator_layer_(std::make_unique<MutatorLayerSlot>()) {
}

AssetStore::AssetStore(
    std::vector<std::filesystem::path> loose_roots, std::size_t loose_index_limit
)
    : loose_roots_(std::move(loose_roots)),
      loose_index_(std::make_unique<LooseIndex>(loose_index_limit)),
      pack_layer_(std::make_unique<PackLayerSlot>()),
      mutator_layer_(std::make_unique<MutatorLayerSlot>()) {
    if (loose_roots_.empty())
        fail("an asset store needs a folder");
    for (auto& root : loose_roots_)
        root = std::filesystem::absolute(root);
}

std::span<const std::filesystem::path> AssetStore::loose_roots() const noexcept {
    return loose_roots_;
}

void AssetStore::observe_lookups(LookupObserver observer) noexcept {
    observer_ = observer;
}

void AssetStore::note_lookup(std::string_view name) const {
    if (observer_.looked_up != nullptr)
        observer_.looked_up(observer_.context, name);
}

std::optional<std::filesystem::path> AssetStore::loose_file(std::string_view resource) const {
    return loose_path(resource);
}

bool AssetStore::loose_index_enabled() const noexcept {
    return loose_index_ && loose_index_->enabled.load();
}

AssetStore::~AssetStore() = default;
AssetStore::AssetStore(AssetStore&&) noexcept = default;
AssetStore& AssetStore::operator=(AssetStore&&) noexcept = default;

bool AssetStore::is_mounted(const std::filesystem::path& archive) const {
    const auto key = full_path_key(archive);
    for (const auto& mounted : mounts_)
        if (full_path_key(mounted.path) == key)
            return true;
    return false;
}

void AssetStore::mount(const std::filesystem::path& archive) {
    if (is_mounted(archive))
        return;
    std::string error;
    if (!try_mount(archive, &error))
        fail(error);
}

bool AssetStore::try_mount(const std::filesystem::path& archive, std::string* error) {
    if (is_mounted(archive)) {
        if (error != nullptr)
            *error = "already mounted";
        return false;
    }
    std::error_code status;
    const auto absolute = std::filesystem::weakly_canonical(archive, status);
    std::string reason;
    if (status)
        reason =
            "cannot resolve HPI archive path '" + utf8_text(archive) + "': " + status.message();
    auto file = status ? nullptr : open_archive_file(absolute, reason);
    if (!file) {
        if (error != nullptr)
            *error = std::move(reason);
        return false;
    }
    auto opened = HpiArchive::open(std::move(file));
    if (!opened.ok()) {
        if (error != nullptr)
            *error = opened.error.message;
        return false;
    }
    mounts_.push_back(Mount{absolute, std::move(*opened.value), {}});
    mount_paths_.push_back(absolute);
    return true;
}

void AssetStore::drop_vanished_mounts() {
    for (std::size_t i = 0; i < mounts_.size();) {
        std::ifstream probe(mounts_[i].path, std::ios::binary);
        if (probe) {
            ++i;
            continue;
        }
        mounts_.erase(mounts_.begin() + static_cast<std::ptrdiff_t>(i));
        mount_paths_.erase(mount_paths_.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

std::vector<DiscoveredArchive> AssetStore::discover(
    std::string_view version, std::span<const std::filesystem::path> removable_roots
) {
    DiscoveryPlan plan;
    plan.revision_archive =
        std::string(kDiscoveryGp3Prefix) + std::string(version) + std::string(kDiscoveryGp3);
    return discover(plan, removable_roots);
}

bool same_archive_file_name(std::string_view left, std::string_view right) {
    return normalized_path(left) == normalized_path(right);
}

std::vector<DiscoveredArchive> AssetStore::discover(
    const DiscoveryPlan& plan, std::span<const std::filesystem::path> removable_roots
) {
    drop_vanished_mounts();
    std::vector<DiscoveredArchive> report;
    // A listed name is pulled out of every group and mounted later, in the
    // list's order. An empty list pulls nothing, so the groups scan as before.
    const bool pulling = !plan.installation_archives.empty();
    const auto named = [&](std::string_view name) {
        for (const auto& archive : plan.installation_archives)
            if (same_archive_file_name(name, archive))
                return true;
        return false;
    };
    // The first item of a listing that is the named archive; null when none is.
    const auto find_named = [](const std::vector<LooseItem>& items,
                               std::string_view archive) -> const LooseItem* {
        for (const auto& item : items)
            if (!dot_entry(item) && same_archive_file_name(item.name, archive))
                return &item;
        return nullptr;
    };
    const auto scan =
        [&](const std::vector<LooseItem>& items, std::string_view pattern, int limit) {
            int remaining = limit;
            for (const auto& item : items) {
                if (dot_entry(item) || !match_host_pattern(item.name, pattern))
                    continue;
                if (pulling && named(item.name))
                    continue;
                // An empty file hides the archive of its name beneath it
                // (oamod-standard 4.1) and is not opened.
                DiscoveredArchive outcome{
                    item.path, false, {}, is_mounted(item.path), empty_file(item)
                };
                if (!outcome.already_mounted && !outcome.hiding)
                    outcome.mounted = try_mount(item.path, &outcome.error);
                report.push_back(std::move(outcome));
                if (report.back().mounted && --remaining == 0)
                    break;
            }
        };
    const auto folders = merged_listing(loose_roots_, true);
    // Each removable root is listed once: the installation archives are
    // sought there after the folders, and its disc scan below reads the same.
    std::vector<std::vector<LooseItem>> disc_listings;
    disc_listings.reserve(removable_roots.size());
    for (const auto& root : removable_roots)
        disc_listings.push_back(loose_listing(root));
    scan(folders, plan.revision_archive, -1);
    scan(folders, plan.ccx_pattern, -1);
    scan(folders, plan.ufo_pattern, -1);
    for (const auto& archive : plan.installation_archives) {
        const LooseItem* found = find_named(folders, archive);
        for (std::size_t index = 0; found == nullptr && index < disc_listings.size(); ++index)
            found = find_named(disc_listings[index], archive);
        if (found == nullptr) {
            // The game folder is the last root. The name is what the report
            // and the skip line show; the file is not there to open.
            std::filesystem::path path{std::u8string(archive.begin(), archive.end())};
            if (!loose_roots_.empty())
                path = loose_roots_.back() / path;
            report.push_back(
                DiscoveredArchive{
                    std::move(path),
                    false,
                    "installation archive '" + archive + "' is missing",
                    false,
                    false
                }
            );
            continue;
        }
        // A named archive is opened even when empty: the profile asks for it.
        DiscoveredArchive outcome{found->path, false, {}, is_mounted(found->path), false};
        if (!outcome.already_mounted)
            outcome.mounted = try_mount(found->path, &outcome.error);
        report.push_back(std::move(outcome));
    }
    scan(folders, plan.hpi_pattern, plan.hpi_limit);
    for (const auto& listing : disc_listings)
        scan(listing, plan.disc_pattern, -1);
    if (plan.folders_as_disc)
        scan(folders, plan.disc_pattern, -1);
    mark_loose_shadows();
    return report;
}

void AssetStore::mark_loose_shadows() {
    if (loose_index_)
        loose_index_->reset();
    for (auto& mounted : mounts_)
        mounted.marks.assign(mounted.archive.nodes().size(), 0);
    if (!mounts_.empty())
        for (const auto& root : loose_roots_) {
            walk_root_ = root;
            walked_folders_.clear();
            mark_loose_directory("", root);
        }
    walked_folders_.clear();
    if (pack_layer_ == nullptr)
        return;
    const auto current = pack_layer_snapshot();
    if (!current)
        return;
    auto updated = std::make_shared<PackLayer>(*current);
    for (auto& entry : updated->entries)
        entry.hidden = loose_path(entry.path).has_value();
    const base::threads::LockGuard guard(pack_layer_->lock);
    if (pack_layer_->layer == current)
        pack_layer_->layer = std::move(updated);
}

void AssetStore::mark_loose_directory(
    const std::string& prefix, const std::filesystem::path& directory
) {
    // A folder reached again through a link, or one a link leads to outside
    // the root, is not walked: a link to a folder above it cannot recurse.
    if (!walked_folders_.insert(resolved(directory)).second ||
        (!prefix.empty() && !stays_inside(walk_root_, directory)))
        return;
    for (const auto& item : loose_listing(directory)) {
        if (item.directory) {
            // A layered folder's hidden folder shadows nothing.
            const bool hidden = prefix.empty() && hidden_at_top(normalized_path(item.name));
            if (item.name != kCurrentDirectory && item.name != kParentDirectory && !hidden)
                mark_loose_directory(prefix + item.name + "\\", item.path);
            continue;
        }
        std::error_code link_error;
        if (std::filesystem::is_symlink(std::filesystem::symlink_status(item.path, link_error)) &&
            !stays_inside(walk_root_, item.path))
            continue;
        const std::string resource = prefix + item.name;
        for (auto& mounted : mounts_) {
            const auto node = mounted.archive.lookup(resource);
            if (node && !mounted.archive.nodes()[*node].directory())
                mounted.marks[*node] |= kShadowedByLoose;
        }
    }
}

std::span<const std::filesystem::path> AssetStore::mount_paths() const noexcept {
    return mount_paths_;
}

const HpiArchive& AssetStore::mounted(std::size_t index) const {
    return mounts_.at(index).archive;
}

std::optional<std::filesystem::path> AssetStore::loose_path(std::string_view resource) const {
    note_lookup(resource);
    if (resource.empty() || resource.front() == '/' || resource.front() == '\\' ||
        resource.find(':') != std::string_view::npos ||
        resource.find('\0') != std::string_view::npos || resource.size() > kMaxPathLength)
        fail("invalid asset resource path");
    const auto key = normalized_path(resource);
    if (hidden_at_top(first_part(key)))
        return std::nullopt;
    for (std::size_t root = 0; root < loose_roots_.size(); ++root)
        if (auto found = loose_path_in(root, key))
            return found;
    return std::nullopt;
}

std::optional<std::filesystem::path>
AssetStore::loose_path_in(std::size_t root, const std::string& key) const {
    if (!loose_index_ || !loose_index_->enabled.load())
        return loose_path_listed_in(root, key);
    const base::threads::LockGuard guard(loose_index_->lock);
    // The folder listed next. It points into a kept listing, which the next
    // listing drops only after it has finished reading the folder.
    const std::filesystem::path* loose = &loose_roots_[root];
    // Each folder's listings are kept under its own index, so that layered
    // folders never answer for each other.
    std::string folder = std::to_string(root) + '|';
    const std::size_t tag = folder.size();
    // Whether a file found lies inside the root is checked as its path is
    // now, not as it was listed: a folder on it may since have become a link.
    std::size_t parts = 0;
    for (std::size_t begin = 0; begin < key.size();) {
        const auto end = key.find('/', begin);
        const auto part = std::string_view(key).substr(
            begin, end == std::string::npos ? std::string_view::npos : end - begin
        );
        if (part == "." || part == "..")
            fail("asset path contains traversal");
        const auto* listing = loose_index_->listing(folder, *loose);
        if (listing == nullptr)
            return loose_index_->enabled.load() ? std::nullopt : loose_path_listed_in(root, key);
        const auto child = listing->children.find(part);
        if (child == listing->children.end())
            return std::nullopt;
        if (child->second.ambiguous)
            fail("ambiguous loose asset case: " + key);
        ++parts;
        if (end == std::string::npos)
            return child->second.regular &&
                           found_inside(loose_roots_[root], child->second.path, parts)
                       ? std::optional(child->second.path)
                       : std::nullopt;
        if (!child->second.directory)
            return std::nullopt;
        loose = &child->second.path;
        folder.resize(tag);
        folder.append(key, 0, end);
        begin = end + 1;
    }
    return std::nullopt;
}

std::optional<std::filesystem::path>
AssetStore::loose_path_listed_in(std::size_t root, const std::string& key) const {
    auto loose = loose_roots_[root];
    for (std::size_t begin = 0; begin < key.size();) {
        const auto end = key.find('/', begin);
        const auto part = key.substr(begin, end == std::string::npos ? end : end - begin);
        if (part == "." || part == "..")
            fail("asset path contains traversal");
        std::error_code error;
        if (!std::filesystem::is_directory(loose, error))
            return std::nullopt;
        std::optional<std::filesystem::path> match;
        // Windows ASCII case-insensitivity on case-sensitive hosts. Ambiguous
        // case collisions are rejected instead of choosing host order.
        for (const auto& item : std::filesystem::directory_iterator(loose, error)) {
            if (normalized_path(utf8_text(item.path().filename())) == part) {
                if (match)
                    fail("ambiguous loose asset case: " + key);
                match = item.path();
            }
        }
        if (!match)
            return std::nullopt;
        loose = *match;
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(loose, error) || !stays_inside(loose_roots_[root], loose))
        return std::nullopt;
    return loose;
}

std::optional<AssetStore::ArchivedNode>
AssetStore::archived_node(std::string_view resource, const std::filesystem::path* skipped) const {
    for (std::size_t index = 0; index < mounts_.size(); ++index) {
        const auto& mounted = mounts_[index];
        if (skipped != nullptr && mounted.path == *skipped)
            continue;
        const auto node = mounted.archive.lookup(resource);
        if (node && !mounted.archive.nodes()[*node].directory())
            return ArchivedNode{index, *node};
    }
    return std::nullopt;
}

AssetData
AssetStore::read_skipping(std::string_view resource, const std::filesystem::path* skipped) const {
    if (const auto hit = MutatorLayerOps::consult(*this, resource); hit.decision.answers()) {
        note_lookup(resource);
        note_mutator(observer_, resource, hit.decision.owner(), hit.decision.hidden());
        if (hit.decision.hidden())
            fail("asset not found: " + normalized_path(resource));
        return MutatorLayerOps::read_entry(*hit.layer, *hit.decision.entry);
    }
    if (const auto loose = loose_path(resource))
        return {read_loose(*loose), *loose, false};
    if (const auto found = archived_node(resource, skipped)) {
        const auto& mounted = mounts_[found->mount];
        auto bytes = mounted.archive.read_node(found->node);
        if (!bytes.ok())
            fail_entry_read(bytes.error, mounted.path, mounted.archive.nodes()[found->node]);
        return {std::move(*bytes.value), mounted.path, true};
    }
    if (const auto layer = pack_layer_snapshot()) {
        const auto listed = layer->index.find(normalized_path(resource));
        if (listed != layer->index.end())
            return PackLayerOps::read_pack_entry(*layer, layer->entries[listed->second]);
    }
    fail("asset not found: " + normalized_path(resource));
}

AssetData AssetStore::read(std::string_view resource) const {
    return read_skipping(resource, nullptr);
}

AssetData
AssetStore::read_without(std::string_view resource, const std::filesystem::path& archive) const {
    return read_skipping(resource, &archive);
}

std::optional<std::filesystem::path>
AssetStore::providing_archive(std::string_view resource) const {
    if (const auto hit = MutatorLayerOps::consult(*this, resource); hit.decision.answers()) {
        note_lookup(resource);
        note_mutator(observer_, resource, hit.decision.owner(), hit.decision.hidden());
        return std::nullopt;
    }
    if (loose_path(resource))
        return std::nullopt;
    if (const auto found = archived_node(resource, nullptr))
        return mounts_[found->mount].path;
    return std::nullopt;
}

std::shared_ptr<const AssetStore::MutatorLayer> AssetStore::mutator_layer_snapshot() const {
    if (mutator_layer_ == nullptr)
        return nullptr;
    const base::threads::LockGuard guard(mutator_layer_->lock);
    return mutator_layer_->layer;
}

std::shared_ptr<const AssetStore::PackLayer> AssetStore::pack_layer_snapshot() const {
    if (pack_layer_ == nullptr)
        return nullptr;
    const base::threads::LockGuard guard(pack_layer_->lock);
    return pack_layer_->layer;
}

bool AssetStore::mount_pack_layer(PackLayerSpec spec, std::string* error) {
    if (pack_layer_ == nullptr)
        pack_layer_ = std::make_unique<PackLayerSlot>();
    if (const auto mounted = pack_layer_snapshot()) {
        if (error != nullptr)
            *error = "a pack layer is mounted already: " + mounted->label;
        return false;
    }
    std::string reason;
    auto built = PackLayerOps::make_pack_layer(spec, reason);
    if (!built) {
        if (error != nullptr)
            *error = std::move(reason);
        return false;
    }
    for (auto& entry : built->entries)
        entry.hidden = loose_path(entry.path).has_value();
    built->serial = next_pack_serial();
    const base::threads::LockGuard guard(pack_layer_->lock);
    if (pack_layer_->layer) {
        if (error != nullptr)
            *error = "a pack layer is mounted already: " + pack_layer_->layer->label;
        return false;
    }
    pack_layer_->layer = std::move(built);
    return true;
}

bool AssetStore::unmount_pack_layer() noexcept {
    if (pack_layer_ == nullptr)
        return false;
    const base::threads::LockGuard guard(pack_layer_->lock);
    const bool mounted = pack_layer_->layer != nullptr;
    pack_layer_->layer.reset();
    return mounted;
}

bool AssetStore::pack_layer_mounted() const {
    return pack_layer_snapshot() != nullptr;
}

std::optional<std::string> AssetStore::pack_layer_label() const {
    const auto layer = pack_layer_snapshot();
    if (!layer)
        return std::nullopt;
    return layer->label;
}

std::vector<std::string> AssetStore::pack_layer_paths() const {
    const auto layer = pack_layer_snapshot();
    std::vector<std::string> paths;
    if (!layer)
        return paths;
    paths.reserve(layer->entries.size());
    for (const auto& entry : layer->entries)
        paths.push_back(entry.path);
    return paths;
}

bool AssetStore::mount_mutator_layer(MutatorLayerSpec spec, std::string* error) {
    if (mutator_layer_ == nullptr)
        mutator_layer_ = std::make_unique<MutatorLayerSlot>();
    if (const auto mounted = mutator_layer_snapshot()) {
        if (error != nullptr)
            *error = "a mutator layer is mounted already: " + mounted->label;
        return false;
    }
    std::string reason;
    auto built = MutatorLayerOps::make(spec, reason);
    if (!built) {
        if (error != nullptr)
            *error = std::move(reason);
        return false;
    }
    built->serial = next_pack_serial();
    const base::threads::LockGuard guard(mutator_layer_->lock);
    if (mutator_layer_->layer) {
        if (error != nullptr)
            *error = "a mutator layer is mounted already: " + mutator_layer_->layer->label;
        return false;
    }
    mutator_layer_->layer = std::move(built);
    return true;
}

bool AssetStore::replace_mutator_layer(MutatorLayerSpec spec, std::string* error) {
    if (mutator_layer_ == nullptr)
        mutator_layer_ = std::make_unique<MutatorLayerSlot>();
    std::string reason;
    auto built = MutatorLayerOps::make(spec, reason);
    if (!built) {
        if (error != nullptr)
            *error = std::move(reason);
        return false;
    }
    built->serial = next_pack_serial();
    const base::threads::LockGuard guard(mutator_layer_->lock);
    mutator_layer_->layer = std::move(built);
    return true;
}

bool AssetStore::unmount_mutator_layer() noexcept {
    if (mutator_layer_ == nullptr)
        return false;
    const base::threads::LockGuard guard(mutator_layer_->lock);
    const bool mounted = mutator_layer_->layer != nullptr;
    mutator_layer_->layer.reset();
    return mounted;
}

bool AssetStore::mutator_layer_mounted() const {
    return mutator_layer_snapshot() != nullptr;
}

std::optional<std::string> AssetStore::mutator_layer_label() const {
    const auto layer = mutator_layer_snapshot();
    if (!layer)
        return std::nullopt;
    return layer->label;
}

std::vector<std::string> AssetStore::mutator_layer_paths() const {
    const auto layer = mutator_layer_snapshot();
    std::vector<std::string> paths;
    if (!layer)
        return paths;
    for (const auto& entry : layer->entries)
        if (entry.kind != MutatorSourceKind::hidden)
            paths.push_back(entry.path);
    return paths;
}

Provider AssetStore::provider(std::string_view resource) const {
    if (const auto hit = MutatorLayerOps::consult(*this, resource); hit.decision.answers()) {
        note_lookup(resource);
        note_mutator(observer_, resource, hit.decision.owner(), hit.decision.hidden());
        if (hit.decision.hidden())
            return {};
        return MutatorLayerOps::provider_of(*hit.layer, *hit.decision.entry);
    }
    if (const auto loose = loose_path(resource))
        return {ProviderKind::loose, *loose, "loose:" + utf8_text(*loose)};
    if (const auto found = archived_node(resource, nullptr)) {
        const auto& path = mounts_[found->mount].path;
        return {ProviderKind::archive, path, "archive:" + utf8_text(path)};
    }
    if (const auto layer = pack_layer_snapshot()) {
        const auto listed = layer->index.find(normalized_path(resource));
        if (listed != layer->index.end())
            return {
                ProviderKind::pack_layer,
                layer->location,
                "pack:" + layer->label + "#" + std::to_string(layer->serial)
            };
    }
    return {};
}

bool AssetStore::provided_above_pack_layer(std::string_view resource) const {
    const auto kind = provider(resource).kind;
    return kind == ProviderKind::loose || kind == ProviderKind::archive ||
           kind == ProviderKind::mutator_layer;
}

std::optional<std::vector<uint8_t>> AssetStore::read_pack_layer(std::string_view resource) const {
    note_lookup(resource);
    if (!pack_path_problem(resource).empty())
        return std::nullopt;
    const auto layer = pack_layer_snapshot();
    if (!layer)
        return std::nullopt;
    const auto listed = layer->index.find(normalized_path(resource));
    if (listed == layer->index.end())
        return std::nullopt;
    return PackLayerOps::read_pack_entry(*layer, layer->entries[listed->second]).bytes;
}

std::optional<std::vector<uint8_t>>
AssetStore::load_file_contents(std::string_view resource) const {
    ResourceFile* file = open(resource);
    if (file == nullptr)
        return std::nullopt;
    std::optional<std::vector<uint8_t>> result;
    const uint32_t size = length(file);
    if (static_cast<int32_t>(size) > 0 && formats::hpi::entry_size_allowed(size) &&
        seek(file, 0) != -1) {
        std::vector<uint8_t> bytes(size);
        if (read(file, bytes) > 0)
            result = std::move(bytes);
    }
    close(file);
    return result;
}

std::vector<uint8_t> AssetStore::load_with_progress(
    std::string_view resource, void (*progress)(void* user, uint8_t percent), void* user
) const {
    constexpr int kPasses = 10;
    constexpr uint8_t kPercentPerPass = 9;
    ResourceFile* file = open(resource);
    if (file == nullptr)
        fail("cannot open " + std::string(resource));
    const uint32_t size = length(file);
    if (!formats::hpi::entry_size_allowed(size)) {
        close(file);
        fail(std::string(resource) + " exceeds the entry size limit");
    }
    std::vector<uint8_t> bytes(size);
    const std::size_t slice = size / static_cast<uint32_t>(kPasses);
    size_t loaded = 0;
    for (int pass = 1; pass <= kPasses; ++pass) {
        const int32_t count = read(file, std::span(bytes).subspan(loaded, slice));
        if (count > 0)
            loaded += static_cast<size_t>(count);
        if (progress != nullptr)
            progress(user, static_cast<uint8_t>(pass * kPercentPerPass));
    }
    if (loaded < size)
        (void)read(file, std::span(bytes).subspan(loaded));
    close(file);
    return bytes;
}

uint32_t AssetStore::file_size(std::string_view resource) const {
    ResourceFile* file = open(resource);
    if (file == nullptr)
        return 0;
    const uint32_t size = length(file);
    close(file);
    return size;
}

bool AssetStore::read_chunk(
    std::string_view resource, uint32_t position, std::span<uint8_t> output
) const {
    ResourceFile* file = open(resource);
    if (file == nullptr)
        return false;
    const bool ok = seek(file, position) != -1 && read(file, output) >= 1;
    close(file);
    return ok;
}

std::vector<FoundEntry> AssetStore::find(std::string_view pattern, FindScope scope) const {
    note_lookup(pattern);
    std::vector<FoundEntry> found;
    std::unordered_set<std::string> hidden_noted;
    const auto mutator = mutator_layer_snapshot();
    const std::size_t split = last_separator(pattern);
    const std::string_view directory = pattern.substr(0, split);
    std::string_view spec = pattern.substr(split);
    if (spec == kAllFilesPattern)
        spec = "*";
    const bool layer_start = scope.first_mount == formats::hpi::MutatorLayerMount;
    const bool loose_start = scope.first_mount < 0 && !layer_start;
    if (mutator && scope.include_mutator_layer && (loose_start || layer_start))
        MutatorLayerOps::append_matches(found, *mutator, directory, spec);
    if (layer_start && !scope.continue_into_mounts)
        return found;
    if (loose_start || layer_start) {
        std::vector<std::filesystem::path> folders;
        const bool roots = directory.empty();
        if (roots) {
            folders = loose_roots_;
        } else {
            const std::string trimmed(directory.substr(0, directory.size() - 1));
            if (!trimmed.empty() && trimmed.find(':') == std::string::npos) {
                const auto key = normalized_path(trimmed);
                for (const auto& root : loose_roots_)
                    if (auto folder = find_loose_directory(root, key))
                        folders.push_back(std::move(*folder));
            }
        }
        if (!folders.empty())
            for (const auto& item : merged_listing(folders, roots)) {
                if (!match_host_pattern(item.name, spec))
                    continue;
                if (!item.directory &&
                    MutatorLayerOps::omit_file(
                        mutator.get(), child_path(directory, item.name), observer_, hidden_noted
                    ))
                    continue;
                found.push_back({item.name, item.directory, item.directory ? 0 : item.size, -1});
            }
        if (!scope.continue_into_mounts)
            return found;
        scope.first_mount = 0;
    }
    find_in_mounts(found, directory, spec, scope, mutator.get(), hidden_noted);
    return found;
}

void AssetStore::find_in_mounts(
    std::vector<FoundEntry>& found,
    std::string_view directory,
    std::string_view spec,
    FindScope scope,
    const MutatorLayer* mutator,
    std::unordered_set<std::string>& hidden_noted
) const {
    for (auto mount = static_cast<std::size_t>(scope.first_mount); mount < mounts_.size();
         ++mount) {
        const auto& archive = mounts_[mount].archive;
        const auto& marks = mounts_[mount].marks;
        if (const auto node = archive.lookup_directory(directory)) {
            const ArchiveNode& parent = archive.nodes()[*node];
            for (uint32_t i = 0; i < parent.child_count; ++i) {
                const auto index = parent.first_child + i;
                const ArchiveNode& child = archive.nodes()[index];
                const bool shadowed =
                    index < marks.size() && (marks[index] & kShadowedByLoose) != 0;
                if (!match_wildcard(child.name, spec) || shadowed)
                    continue;
                if (!child.directory() &&
                    MutatorLayerOps::omit_file(
                        mutator, child_path(directory, child.name), observer_, hidden_noted
                    ))
                    continue;
                found.push_back(
                    {child.name,
                     child.directory(),
                     child.directory() ? 0 : child.size,
                     static_cast<int>(mount)}
                );
            }
        }
        if (!scope.continue_into_mounts)
            return;
    }
    if (!scope.include_pack_layer)
        return;
    const auto start = scope.first_mount < 0 ? 0 : scope.first_mount;
    if (start > static_cast<int>(mounts_.size()))
        return;
    if (start < static_cast<int>(mounts_.size()) && !scope.continue_into_mounts)
        return;
    const auto layer = pack_layer_snapshot();
    if (!layer)
        return;
    std::vector<FoundEntry> pack_found;
    PackLayerOps::append_pack_matches(
        pack_found, *layer, directory, spec, static_cast<int>(mounts_.size())
    );
    for (auto& entry : pack_found) {
        if (!entry.directory &&
            MutatorLayerOps::omit_file(
                mutator, child_path(directory, entry.name), observer_, hidden_noted
            ))
            continue;
        found.push_back(std::move(entry));
    }
}

std::vector<std::string> AssetStore::scan_recursive(
    std::string_view directory, std::string_view pattern, FindScope scope
) const {
    std::vector<std::string> result;
    const auto visit = [&](const auto& self, const std::string& path, FindScope from) -> void {
        for (const auto& entry : find(path + "\\*", from)) {
            if (entry.name == kCurrentDirectory || entry.name == kParentDirectory)
                continue;
            const std::string child = path + "\\" + entry.name;
            if (!entry.directory) {
                if (match_wildcard(entry.name, pattern))
                    result.push_back(child);
                continue;
            }
            const FindScope next = entry.mutator_layer
                                       ? FindScope{formats::hpi::MutatorLayerMount, false}
                                       : FindScope{entry.mount, false, true, false};
            self(self, child, next);
        }
    };
    // An empty directory names the drive root in the game; nothing here.
    if (!directory.empty())
        visit(visit, std::string(directory), scope);
    return result;
}

int AssetStore::count_entries(std::string_view pattern, bool directories_only) const {
    int count = 0;
    for (const auto& entry : find(pattern))
        if (entry.name != kCurrentDirectory && entry.name != kParentDirectory &&
            (!directories_only || entry.directory))
            ++count;
    return count;
}

ResourceFile* AssetStore::open(std::string_view resource) const {
    if (const auto hit = MutatorLayerOps::consult(*this, resource); hit.decision.answers()) {
        note_lookup(resource);
        note_mutator(observer_, resource, hit.decision.owner(), hit.decision.hidden());
        if (hit.decision.hidden())
            return nullptr;
        return MutatorLayerOps::open_entry(hit.layer, *hit.decision.entry);
    }
    if (const auto loose = loose_path(resource)) {
        auto* file = new ResourceFile;
        file->loose.open(*loose, std::ios::binary);
        if (file->loose) {
            std::error_code error;
            const auto size = std::filesystem::file_size(*loose, error);
            file->loose_size =
                error ? 0 : static_cast<uint32_t>(std::min<uintmax_t>(size, 0xFFFFFFFFU));
            return file;
        }
        delete file;
    }
    for (const auto& mounted : mounts_) {
        const auto node = mounted.archive.lookup(resource);
        if (!node || mounted.archive.nodes()[*node].directory())
            continue;
        auto* file = new ResourceFile;
        file->archive = &mounted.archive;
        file->archive_path = &mounted.path;
        file->node = *node;
        file->size = mounted.archive.nodes()[*node].size;
        return file;
    }
    if (const auto layer = pack_layer_snapshot()) {
        const auto listed = layer->index.find(normalized_path(resource));
        if (listed == layer->index.end())
            return nullptr;
        const auto& entry = layer->entries[listed->second];
        auto* file = new ResourceFile;
        file->pack_layer = layer;
        if (entry.contents || layer->kind == PackLayerKind::zip) {
            try {
                file->memory =
                    entry.contents ? *entry.contents : PackLayerOps::read_pack_zip(*layer, entry);
            } catch (...) {
                delete file;
                throw;
            }
            file->in_memory = true;
            file->size = static_cast<uint32_t>(file->memory.size());
            return file;
        }
        file->loose.open(entry.host, std::ios::binary);
        if (!file->loose) {
            const auto label = layer->label;
            const auto path = entry.path;
            delete file;
            fail("pack layer '" + label + "' cannot read '" + path + "': cannot open the file");
        }
        file->loose_size = entry.size;
        file->size = entry.size;
        return file;
    }
    return nullptr;
}

void AssetStore::close(ResourceFile* file) noexcept {
    delete file;
}

bool AssetStore::archived(const ResourceFile* file) noexcept {
    return file->archive != nullptr || file->memory_archived ||
           (file->pack_layer != nullptr && file->pack_layer->kind == PackLayerKind::zip);
}

int32_t AssetStore::seek(ResourceFile* file, uint32_t position) {
    if (file->in_memory) {
        if (position == kSeekToEnd)
            position = file->size;
        file->position = position;
        return 0;
    }
    if (file->archive == nullptr) {
        file->loose.clear();
        if (position == kSeekToEnd)
            file->loose.seekg(0, std::ios::end);
        else
            file->loose.seekg(static_cast<std::streamoff>(position));
        if (!file->loose)
            return -1;
        return static_cast<int32_t>(file->loose.tellg());
    }
    const uint32_t previous = file->position;
    if (position == kSeekToEnd)
        position = file->size;
    file->position = position;
    if (((previous ^ position) & ~(formats::hpi::BlockBytes - 1U)) != 0)
        file->block.clear();
    return 0;
}

int32_t AssetStore::tell(const ResourceFile* file) {
    if (file->in_memory)
        return static_cast<int32_t>(file->position);
    if (file->archive == nullptr)
        return static_cast<int32_t>(const_cast<ResourceFile*>(file)->loose.tellg());
    return static_cast<int32_t>(file->position);
}

uint32_t AssetStore::length(const ResourceFile* file) {
    if (file->in_memory)
        return file->size;
    return file->archive == nullptr ? file->loose_size : file->size;
}

int32_t AssetStore::read(ResourceFile* file, std::span<uint8_t> output) {
    if (file->in_memory) {
        if (file->position >= file->size || output.empty())
            return 0;
        const auto count = std::min<std::size_t>(
            output.size(), static_cast<std::size_t>(file->size - file->position)
        );
        std::copy_n(
            file->memory.begin() + static_cast<std::ptrdiff_t>(file->position),
            count,
            output.begin()
        );
        file->position += static_cast<uint32_t>(count);
        return static_cast<int32_t>(count);
    }
    if (file->archive == nullptr) {
        file->loose.read(
            reinterpret_cast<char*>(output.data()), static_cast<std::streamsize>(output.size())
        );
        const auto count = file->loose.gcount();
        file->loose.clear();
        return static_cast<int32_t>(count);
    }
    if (file->position >= file->size)
        return 0;
    const std::size_t wanted = std::min<std::size_t>(output.size(), file->size - file->position);
    const ArchiveNode& node = file->archive->nodes()[file->node];
    if (node.compression == 0) {
        const auto count =
            file->archive->read_node_range(file->node, file->position, output.first(wanted));
        if (!count.ok())
            fail_entry_read(count.error, *file->archive_path, node);
        file->position += *count.value;
        return static_cast<int32_t>(*count.value);
    }
    std::size_t copied = 0;
    while (copied < wanted) {
        const uint32_t base = file->position & ~(formats::hpi::BlockBytes - 1U);
        if (file->block.empty()) {
            if (!file->chunks) {
                auto table = file->archive->read_chunk_table(file->node);
                // A table that cannot be read ends the read, as a chunk does.
                if (!table.ok()) {
                    if (table.error.code == base::bytes::DecodeCode::truncated)
                        return -1;
                    fail_entry_read(table.error, *file->archive_path, node);
                }
                file->chunks = std::move(*table.value);
            }
            file->block.assign(std::min<uint32_t>(formats::hpi::BlockBytes, file->size - base), 0);
            const auto read =
                file->archive->read_node_range(file->node, base, file->block, *file->chunks);
            if (!read.ok()) {
                file->block.clear();
                // A chunk that cannot be read ends the read; one that fails
                // to decode is fatal, as in 3.1c.
                if (read.error.code == base::bytes::DecodeCode::truncated)
                    return -1;
                fail_entry_read(read.error, *file->archive_path, node);
            }
        }
        const uint32_t offset = file->position - base;
        const std::size_t take =
            std::min<std::size_t>(file->block.size() - offset, wanted - copied);
        std::copy_n(
            file->block.begin() + offset, take, output.begin() + static_cast<std::ptrdiff_t>(copied)
        );
        copied += take;
        (void)seek(file, file->position + static_cast<uint32_t>(take));
    }
    return static_cast<int32_t>(copied);
}

base::bytes::Decoded<HpiArchive> open_hpi_file(const std::filesystem::path& path) {
    std::string reason;
    auto file = open_archive_file(path, reason);
    if (!file)
        return base::bytes::DecodeError{
            base::bytes::DecodeCode::not_found, 0, "cannot open HPI archive"
        };
    return HpiArchive::open(std::move(file));
}

int32_t write_loose_file(const std::filesystem::path& path, std::span<const uint8_t> bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream)
        return -1;
    stream.write(
        reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
    );
    return stream ? static_cast<int32_t>(bytes.size()) : 0;
}

void create_directory_path(const std::filesystem::path& path) {
    // The path's own spelling, UTF-16 on Windows, so that no name is read in
    // a code page.
    const std::filesystem::path::string_type& text = path.native();
    std::error_code ignored;
    for (std::size_t i = 0; i < text.size(); ++i)
        if (text[i] == '\\' || text[i] == '/')
            std::filesystem::create_directory(std::filesystem::path(text.substr(0, i)), ignored);
    std::filesystem::create_directory(path, ignored);
}

std::vector<std::string> AssetStore::list_effective(
    std::string_view directory, std::string_view extension, bool include_pack_layer
) const {
    auto result = list_effective_in_mount_order(directory, extension, include_pack_layer);
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<std::string> AssetStore::list_effective_in_mount_order(
    std::string_view directory, std::string_view extension, bool include_pack_layer
) const {
    return list_resources(directory, extension, false, include_pack_layer);
}

std::vector<std::string> AssetStore::list_effective_recursive(
    std::string_view directory, std::string_view extension, bool include_pack_layer
) const {
    // Feature loading retains this enumeration order; the first document
    // containing a requested section wins.
    return list_resources(directory, extension, true, include_pack_layer);
}

std::vector<std::string> AssetStore::list_resources(
    std::string_view directory, std::string_view extension, bool recursive, bool include_pack_layer
) const {
    if (directory.size() > kMaxPathLength ||
        directory.find_first_of("\\:\0", 0, 3) != std::string_view::npos ||
        (!directory.empty() && directory.front() == '/') ||
        extension.find_first_of("/\\:\0", 0, 4) != std::string_view::npos)
        fail("invalid asset listing path");
    note_lookup(directory);
    auto prefix = normalized_path(directory);
    for (std::size_t begin = 0; begin < prefix.size();) {
        const auto end = prefix.find('/', begin);
        const auto part = prefix.substr(begin, end == std::string::npos ? end : end - begin);
        if (part.empty() || part == "." || part == "..")
            fail("asset listing contains traversal");
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    // The folder below each layered root that the listing names.
    const auto folder_in = [&](const std::filesystem::path& root) {
        std::optional<std::filesystem::path> loose = root;
        if (hidden_at_top(first_part(prefix)))
            return std::optional<std::filesystem::path>{};
        for (std::size_t begin = 0; loose && begin < prefix.size();) {
            const auto end = prefix.find('/', begin);
            const auto part = prefix.substr(begin, end == std::string::npos ? end : end - begin);
            std::optional<std::filesystem::path> match;
            if (std::filesystem::is_directory(*loose))
                for (const auto& item : std::filesystem::directory_iterator(*loose))
                    if (normalized_path(utf8_text(item.path().filename())) == part) {
                        if (match)
                            fail("ambiguous loose asset directory: " + prefix);
                        match = item.path();
                    }
            loose = match;
            if (end == std::string::npos)
                break;
            begin = end + 1;
        }
        return loose;
    };
    if (!prefix.empty())
        prefix += '/';
    const auto suffix = normalized_path(extension);
    const auto matches = [&](std::string_view key) {
        return key.starts_with(prefix) &&
               (recursive || key.find('/', prefix.size()) == std::string_view::npos) &&
               (suffix.empty() || key.ends_with(suffix));
    };
    std::set<std::string> keys;
    std::vector<std::string> result;
    const auto mutator = mutator_layer_snapshot();
    if (mutator != nullptr)
        for (const auto& entry : mutator->entries) {
            if (entry.kind == MutatorSourceKind::hidden)
                continue;
            if (matches(entry.key) && keys.insert(entry.key).second)
                result.push_back(entry.key);
        }
    const auto claim = [&](const std::string& key) {
        if (!keys.insert(key).second)
            return true;
        if (mutator == nullptr)
            return false;
        const auto decision = MutatorLayerOps::decide(*mutator, key);
        if (!decision.answers())
            return false;
        if (decision.hidden())
            note_mutator(observer_, key, decision.owner(), true);
        return true;
    };
    for (const auto& root : loose_roots_) {
        const auto found = folder_in(root);
        if (!found || !std::filesystem::is_directory(*found) ||
            (!prefix.empty() && !stays_inside(root, *found)))
            continue;
        const auto& loose = *found;
        // Names that differ only in case are ambiguous within one folder;
        // across layered folders they are one file.
        std::set<std::string> folder_keys;
        const auto append = [&](const auto& item) {
            std::error_code link_error;
            if (!item.is_regular_file() ||
                (item.is_symlink(link_error) && !stays_inside(root, item.path())))
                return;
            // Keys spell names in UTF-8, as the lookups compare them.
            const auto relative = item.path().lexically_relative(loose).generic_u8string();
            const auto key =
                prefix + normalized_path(std::string(relative.begin(), relative.end()));
            if (matches(key)) {
                if (!folder_keys.insert(key).second)
                    fail("ambiguous loose asset case: " + key);
                if (!claim(key))
                    result.push_back(key);
            }
        };
        if (recursive) {
            // From a layered folder's top, its hidden folder is not walked.
            const bool at_top = prefix.empty();
            for (auto item = std::filesystem::recursive_directory_iterator(loose);
                 item != std::filesystem::recursive_directory_iterator();
                 ++item) {
                if (at_top && item.depth() == 0 && item->is_directory() &&
                    hidden_at_top(normalized_path(utf8_text(item->path().filename())))) {
                    item.disable_recursion_pending();
                    continue;
                }
                append(*item);
            }
        } else
            for (const auto& item : std::filesystem::directory_iterator(loose))
                append(item);
    }
    // Names only: read() resolves the winning content in the same mount order.
    const std::string_view folder =
        std::string_view(prefix).substr(0, prefix.empty() ? 0 : prefix.size() - 1);
    for (const auto& mounted : mounts_)
        archive_listing(mounted.archive, folder, recursive, [&](std::string key) {
            if (matches(key) && !claim(key))
                result.push_back(std::move(key));
        });
    if (include_pack_layer)
        if (const auto layer = pack_layer_snapshot())
            for (const auto& entry : layer->entries)
                if (matches(entry.key) && !claim(entry.key))
                    result.push_back(entry.key);
    return result;
}

namespace {

/// Returns why a mask pattern is refused, or null when it is valid.
///
/// Allocates nothing, so a noexcept caller can use it.
///
/// @param pattern the pattern
/// @return a static reason, or null
const char* mask_problem(std::string_view pattern) noexcept {
    if (pattern.empty())
        return "empty pattern";
    if (pattern.size() > formats::hpi::MutatorMaskByteLimit)
        return "pattern is too long";
    if (pattern.front() == '/' || pattern.front() == '\\')
        return "absolute pattern";
    if (pattern.find(':') != std::string_view::npos)
        return "pattern contains ':'";
    if (pattern.find('\0') != std::string_view::npos)
        return "pattern contains NUL";
    std::size_t wildcards = 0;
    bool first = true;
    for (std::size_t begin = 0; begin <= pattern.size();) {
        std::size_t end = begin;
        while (end < pattern.size() && pattern[end] != '/' && pattern[end] != '\\')
            ++end;
        const auto segment = pattern.substr(begin, end - begin);
        if (segment.empty() || segment == "." || segment == "..")
            return "pattern contains an empty or traversal segment";
        if (first) {
            if (segment.size() == backup_folder_name.size()) {
                bool backup = true;
                for (std::size_t index = 0; index < segment.size(); ++index) {
                    if (ascii_lower(segment[index]) != backup_folder_name[index]) {
                        backup = false;
                        break;
                    }
                }
                if (backup)
                    return "pattern starts with the backup folder";
            }
            first = false;
        }
        if (segment.find("**") != std::string_view::npos && segment != "**")
            return "'**' is not a whole segment";
        for (const char character : segment)
            if (character == '*' || character == '?')
                ++wildcards;
        if (end >= pattern.size())
            break;
        begin = end + 1;
    }
    if (wildcards > formats::hpi::MutatorMaskWildcardLimit)
        return "too many wildcards";
    return nullptr;
}

/// One '/' separated piece of a folded path, as offsets into that path.
struct FoldedSegment {
    std::size_t begin = 0;
    std::size_t length = 0;
};

/// Splits a folded path into segments. The path has no empty segment.
///
/// @param text the path, '/' separated and ASCII lower case
/// @param[out] segments the pieces
void split_folded(std::string_view text, std::vector<FoldedSegment>& segments) {
    for (std::size_t begin = 0; begin < text.size();) {
        const auto end = text.find('/', begin);
        const auto length = end == std::string_view::npos ? text.size() - begin : end - begin;
        segments.push_back({begin, length});
        if (end == std::string_view::npos)
            break;
        begin = end + 1;
    }
}

/// Returns whether one pattern segment matches one path segment.
///
/// Both are ASCII lower case and hold no '/'. '*' matches any run and '?'
/// one character. The match is a scan over the two lengths, not a search
/// that branches on every wildcard.
///
/// @param pattern the pattern segment
/// @param text the path segment
/// @return true when the whole segment matches
bool segment_matches(std::string_view pattern, std::string_view text) {
    // prev[j] is true when the pattern consumed so far matches text[0, j).
    std::vector<char> previous(text.size() + 1, 0);
    std::vector<char> next(text.size() + 1, 0);
    previous[0] = 1;
    for (const char wanted : pattern) {
        std::fill(next.begin(), next.end(), 0);
        if (wanted == '*') {
            char reached = 0;
            for (std::size_t index = 0; index < next.size(); ++index) {
                reached = static_cast<char>(reached | previous[index]);
                next[index] = reached;
            }
        } else {
            for (std::size_t index = 1; index < next.size(); ++index) {
                const char have = text[index - 1];
                if (previous[index - 1] != 0 && (wanted == '?' || wanted == have))
                    next[index] = 1;
            }
        }
        previous.swap(next);
    }
    return previous.back() != 0;
}

/// Returns whether folded pattern segments match folded path segments.
///
/// A "**" segment matches any run of path segments, including none. The
/// scan walks each pattern segment once and remembers which path segments
/// are reachable, so a run of "**" does not branch on every split.
///
/// @param pattern the folded pattern
/// @param pattern_segments its segments
/// @param path the folded path
/// @param path_segments its segments
/// @return true when the whole path matches
bool segments_match(
    std::string_view pattern,
    const std::vector<FoldedSegment>& pattern_segments,
    std::string_view path,
    const std::vector<FoldedSegment>& path_segments
) {
    std::vector<char> reachable(path_segments.size() + 1, 0);
    reachable[0] = 1;
    for (const auto& piece : pattern_segments) {
        const auto token = pattern.substr(piece.begin, piece.length);
        std::vector<char> next(reachable.size(), 0);
        if (token == "**") {
            char reached = 0;
            for (std::size_t index = 0; index < reachable.size(); ++index) {
                reached = static_cast<char>(reached | reachable[index]);
                next[index] = reached;
            }
        } else {
            for (std::size_t index = 0; index < path_segments.size(); ++index) {
                if (reachable[index] == 0)
                    continue;
                const auto& path_piece = path_segments[index];
                if (segment_matches(token, path.substr(path_piece.begin, path_piece.length)))
                    next[index + 1] = 1;
            }
        }
        reachable.swap(next);
    }
    return reachable.back() != 0;
}

} // namespace

std::string mutator_mask_problem(std::string_view pattern) {
    if (const char* problem = mask_problem(pattern))
        return problem;
    return {};
}

bool mutator_mask_matches(std::string_view pattern, std::string_view path) noexcept {
    if (mask_problem(pattern) != nullptr)
        return false;
    try {
        const auto folded_pattern = normalized_path(pattern);
        const auto folded_path = normalized_path(path);
        std::vector<FoldedSegment> pattern_segments;
        std::vector<FoldedSegment> path_segments;
        split_folded(folded_pattern, pattern_segments);
        split_folded(folded_path, path_segments);
        return segments_match(folded_pattern, pattern_segments, folded_path, path_segments);
    } catch (const std::bad_alloc&) {
        return false;
    }
}

} // namespace oa
