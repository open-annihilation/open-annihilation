// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Copying a base game and its expansions into the player's folder
// (content_copy.hpp). A folder is read as it stands, including a Wineskin
// drive_c, and a Windows installer supplies the same reads. Blocks are
// shared where the volume can do that; otherwise the bytes are copied. A
// stopped copy is continued from the bytes already written. The source is
// only read. The setup's scan recognises a published installer from its
// size, its name and a hash of its first and last megabyte; the copy and
// the installer check still read every byte.
#include "oa/app/content_copy.hpp"
#include "oa/app/disk_image_source.hpp"

#include "oa/app/game_directory.hpp"
#include "oa/base/sha256.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/platform/file_clone.hpp"
#include "oa/platform/files.hpp"
#include "oa/platform/game_installs.hpp"
#include "oa/platform/preferences.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <limits>
#include <optional>
#include <ostream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#include <stdio.h>
#else
#include <unistd.h>
#endif

namespace oa::app {
namespace {

namespace bundles = oa::data::content_bundles;
namespace fs = std::filesystem;

constexpr int k_search_depth = 8;
constexpr std::size_t k_visit_limit = 20000;
constexpr std::size_t k_file_limit = 100000;

[[nodiscard]] bool folder_is_not_searched(const fs::path& folder);

constexpr int64_t k_check_time = 1600000000;

void set_error(std::string* error, std::string_view text) {
    if (error != nullptr)
        *error = std::string(text);
}

std::string player_error(std::string_view detail) {
    std::string text = "The game could not be copied into Open Annihilation";
    if (!detail.empty()) {
        text.append(": ");
        text.append(detail);
    }
    text.push_back('.');
    return text;
}

uint64_t narrow_size(uintmax_t value) noexcept {
    constexpr auto limit = static_cast<uintmax_t>(std::numeric_limits<uint64_t>::max());
    if (value > limit)
        return std::numeric_limits<uint64_t>::max();
    return static_cast<uint64_t>(value);
}

void add_bytes(uint64_t& total, uint64_t more) noexcept {
    if (total > std::numeric_limits<uint64_t>::max() - more)
        total = std::numeric_limits<uint64_t>::max();
    else
        total += more;
}

uint32_t narrow_count(std::size_t count) noexcept {
    constexpr auto limit = static_cast<std::size_t>(std::numeric_limits<uint32_t>::max());
    if (count > limit)
        return std::numeric_limits<uint32_t>::max();
    return static_cast<uint32_t>(count);
}

/// Converts a file time to whole seconds since 1970, as the import does.
template <typename Clock>
[[nodiscard]] int64_t seconds_of_clock(typename Clock::time_point time) {
    const auto count = [](auto system_time) {
        return std::chrono::floor<std::chrono::seconds>(system_time).time_since_epoch().count();
    };
    if constexpr (requires { Clock::to_sys(time); })
        return static_cast<int64_t>(count(Clock::to_sys(time)));
    else {
        const auto utc = Clock::to_utc(time);
        using Utc = typename decltype(utc)::clock;
        return static_cast<int64_t>(count(Utc::to_sys(utc)));
    }
}

/// Converts whole seconds since 1970 to a file time.
template <typename Clock>
[[nodiscard]] typename Clock::time_point time_of_clock(int64_t seconds) {
    const std::chrono::sys_seconds system{std::chrono::seconds{seconds}};
    if constexpr (requires { Clock::from_sys(system); }) {
        return std::chrono::time_point_cast<typename Clock::duration>(Clock::from_sys(system));
    } else {
        using Utc = typename decltype(Clock::to_utc(typename Clock::time_point{}))::clock;
        return std::chrono::time_point_cast<typename Clock::duration>(
            Clock::from_utc(Utc::from_sys(system))
        );
    }
}

[[nodiscard]] int64_t modified_seconds(fs::file_time_type time) {
    return seconds_of_clock<fs::file_time_type::clock>(time);
}

[[nodiscard]] fs::file_time_type time_from_seconds(int64_t seconds) {
    return time_of_clock<fs::file_time_type::clock>(seconds);
}

[[nodiscard]] char fold_byte(char byte) noexcept {
    if (byte == '\\')
        return '/';
    if (byte >= 'A' && byte <= 'Z')
        return static_cast<char>(byte - 'A' + 'a');
    return byte;
}

[[nodiscard]] bool folded_equal(std::string_view one, std::string_view other) noexcept {
    if (one.size() != other.size())
        return false;
    for (std::size_t index = 0; index < one.size(); ++index) {
        if (fold_byte(one[index]) != fold_byte(other[index]))
            return false;
    }
    return true;
}

[[nodiscard]] bool has_separator(std::string_view path) noexcept {
    for (char byte : path) {
        if (byte == '/' || byte == '\\')
            return true;
    }
    return false;
}

[[nodiscard]] std::string_view last_component(std::string_view path) noexcept {
    std::size_t start = 0;
    for (std::size_t index = 0; index < path.size(); ++index) {
        if (path[index] == '/' || path[index] == '\\')
            start = index + 1;
    }
    return path.substr(start);
}

[[nodiscard]] bool names_match(std::string_view listed, const bundles::ContentFile& file) noexcept {
    if (folded_equal(listed, file.copy_name))
        return true;
    for (std::string_view source_name : file.source_names) {
        if (folded_equal(listed, source_name))
            return true;
    }
    if (has_separator(listed))
        return false;
    if (folded_equal(listed, last_component(file.copy_name)))
        return true;
    for (std::string_view source_name : file.source_names) {
        if (folded_equal(listed, last_component(source_name)))
            return true;
    }
    return false;
}

struct TableHit {
    const bundles::Bundle* bundle{};
    const bundles::ContentFile* file{};
};

[[nodiscard]] TableHit match_listed(
    std::span<const bundles::Bundle> table, std::string_view name, uint64_t size
) noexcept {
    for (const bundles::Bundle& bundle : table) {
        for (const bundles::ContentFile& file : bundle.files) {
            if (file.size != size)
                continue;
            if (names_match(name, file))
                return {&bundle, &file};
        }
    }
    return {};
}

[[nodiscard]] bool safe_copy_name(std::string_view name) noexcept {
    if (name.empty() || name.front() == '/' || name.front() == '\\')
        return false;
    std::string part;
    for (char byte : name) {
        if (byte == '/' || byte == '\\') {
            if (part.empty() || part == "." || part == "..")
                return false;
            part.clear();
        } else {
            part.push_back(byte);
        }
    }
    return !part.empty() && part != "." && part != "..";
}

[[nodiscard]] bool split_relative(std::string_view name, std::vector<std::string>& parts) {
    parts.clear();
    std::string current;
    for (char byte : name) {
        if (byte == '/' || byte == '\\') {
            if (current.empty() || current == "." || current == "..")
                return false;
            parts.push_back(std::move(current));
            current.clear();
        } else {
            current.push_back(byte);
        }
    }
    if (current.empty() || current == "." || current == "..")
        return false;
    parts.push_back(std::move(current));
    return true;
}

struct OwnedFile {
    std::FILE* file = nullptr;

    ~OwnedFile() {
        if (file != nullptr)
            std::fclose(file);
    }

    OwnedFile() = default;

    explicit OwnedFile(std::FILE* opened) : file(opened) {}

    OwnedFile(const OwnedFile&) = delete;
    OwnedFile& operator=(const OwnedFile&) = delete;
};

[[nodiscard]] bool sync_file(std::FILE* file) noexcept {
    if (std::fflush(file) != 0)
        return false;
#if defined(_WIN32)
    return _commit(_fileno(file)) == 0;
#else
    return ::fsync(::fileno(file)) == 0;
#endif
}

[[nodiscard]] bool disk_is_full(int number) noexcept {
#if defined(EDQUOT)
    if (number == EDQUOT)
        return true;
#endif
    return number == ENOSPC;
}

[[nodiscard]] bool stopped_now(const std::atomic<bool>* stop) noexcept {
    return stop != nullptr && stop->load();
}

[[nodiscard]] std::string fold_path_text(std::string text) {
    for (char& byte : text) {
        if (byte == '\\')
            byte = '/';
        else if (byte >= 'A' && byte <= 'Z')
            byte = static_cast<char>(byte - 'A' + 'a');
    }
    while (!text.empty() && text.back() == '/')
        text.pop_back();
    return text;
}

[[nodiscard]] std::string canonical_text(const fs::path& path) {
    std::error_code failure;
    fs::path resolved = fs::weakly_canonical(path, failure);
    if (failure)
        resolved = fs::absolute(path, failure);
    if (failure)
        resolved = path;
    return fold_path_text(path_to_utf8(resolved.lexically_normal()));
}

/// Reports whether `candidate` is `directory` or a file inside it.
[[nodiscard]] bool path_is_inside(const fs::path& candidate, const fs::path& directory) {
    if (candidate.empty() || directory.empty())
        return false;
    const std::string child = canonical_text(candidate);
    const std::string parent = canonical_text(directory);
    if (parent.empty())
        return false;
    if (child == parent)
        return true;
    if (child.size() <= parent.size())
        return false;
    return child.compare(0, parent.size(), parent) == 0 && child[parent.size()] == '/';
}

[[nodiscard]] fs::path existing_ancestor(const fs::path& folder) {
    std::error_code failure;
    fs::path current = folder;
    while (!current.empty() && !fs::exists(current, failure)) {
        const fs::path parent = current.parent_path();
        if (parent == current)
            break;
        current = std::move(parent);
    }
    return current;
}

struct Level {
    fs::path path;
    int depth = 0;
};

[[nodiscard]] bool
has_known_child(const fs::path& directory, std::span<const bundles::Bundle> table) {
    std::error_code failure;
    fs::directory_iterator cursor(
        directory, fs::directory_options::skip_permission_denied, failure
    );
    if (failure)
        return false;
    const fs::directory_iterator end;
    while (cursor != end) {
        const fs::path child = cursor->path();
        std::error_code status_error;
        const fs::file_status status = cursor->symlink_status(status_error);
        cursor.increment(failure);
        if (status_error || fs::is_symlink(status) || !fs::is_regular_file(status)) {
            if (failure)
                break;
            continue;
        }
        std::error_code size_error;
        const auto size = fs::file_size(child, size_error);
        if (!size_error &&
            match_listed(table, path_to_utf8(child.filename()), narrow_size(size)).file != nullptr)
            return true;
        if (failure)
            break;
    }
    return false;
}

[[nodiscard]] fs::path
find_named_directory(const fs::path& start, std::string_view wanted, int max_depth) {
    std::vector<Level> pending;
    pending.push_back(Level{start, 0});
    std::size_t cursor = 0;
    std::size_t visits = 0;
    while (cursor < pending.size() && visits < k_visit_limit) {
        const Level current = pending[cursor++];
        ++visits;
        std::error_code failure;
        fs::directory_iterator it(
            current.path, fs::directory_options::skip_permission_denied, failure
        );
        if (failure)
            continue;
        const fs::directory_iterator end;
        while (it != end && visits < k_visit_limit) {
            const fs::path child = it->path();
            std::error_code status_error;
            const fs::file_status status = it->symlink_status(status_error);
            it.increment(failure);
            if (status_error || fs::is_symlink(status) || !fs::is_directory(status)) {
                if (failure)
                    break;
                continue;
            }
            if (folder_is_not_searched(child)) {
                if (failure)
                    break;
                continue;
            }
            if (folded_equal(path_to_utf8(child.filename()), wanted))
                return child;
            if (current.depth + 1 < max_depth)
                pending.push_back(Level{child, current.depth + 1});
            if (failure)
                break;
        }
    }
    return {};
}

[[nodiscard]] int depth_between(const fs::path& parent, const fs::path& child) {
    const fs::path relative = child.lexically_relative(parent);
    int depth = 0;
    for (const fs::path& part : relative) {
        if (part == "..")
            return 0;
        if (part != ".")
            ++depth;
    }
    return depth;
}

[[nodiscard]] fs::path find_shallow_root(
    const fs::path& start, int start_depth, int max_depth, std::span<const bundles::Bundle> table
) {
    std::vector<Level> pending;
    pending.push_back(Level{start, start_depth});
    std::size_t cursor = 0;
    std::size_t visits = 0;
    while (cursor < pending.size() && visits < k_visit_limit) {
        const Level current = pending[cursor++];
        ++visits;
        if (has_known_child(current.path, table))
            return current.path;
        if (current.depth >= max_depth)
            continue;
        std::error_code failure;
        fs::directory_iterator it(
            current.path, fs::directory_options::skip_permission_denied, failure
        );
        if (failure)
            continue;
        const fs::directory_iterator end;
        while (it != end && visits < k_visit_limit) {
            const fs::path child = it->path();
            std::error_code status_error;
            const fs::file_status status = it->symlink_status(status_error);
            it.increment(failure);
            if (!status_error && !fs::is_symlink(status) && fs::is_directory(status) &&
                !folder_is_not_searched(child))
                pending.push_back(Level{child, current.depth + 1});
            if (failure)
                break;
        }
    }
    return {};
}

[[nodiscard]] fs::path
find_game_root(const fs::path& folder, std::span<const bundles::Bundle> table) {
    if (has_known_child(folder, table))
        return folder;
    const fs::path drive = find_named_directory(folder, "drive_c", k_search_depth);
    const fs::path search_from = drive.empty() ? folder : drive;
    const int start_depth = drive.empty() ? 0 : depth_between(folder, drive);
    const fs::path found = find_shallow_root(search_from, start_depth, k_search_depth, table);
    if (!found.empty())
        return found;
    return folder;
}

[[nodiscard]] std::string relative_posix(const fs::path& root, const fs::path& file) {
    const fs::path relative = file.lexically_relative(root);
    std::string name;
    for (const fs::path& part : relative) {
        if (part == "." || part == "..")
            return {};
        if (!name.empty())
            name.push_back('/');
        name += path_to_utf8(part);
    }
    return name;
}

[[nodiscard]] bool
list_tree(const fs::path& root, std::vector<ContentSourceFile>& files, std::string* error) {
    files.clear();
    std::vector<Level> pending;
    pending.push_back(Level{root, 0});
    std::size_t cursor = 0;
    std::size_t visits = 0;
    bool saw_root = false;
    while (cursor < pending.size() && visits < k_visit_limit && files.size() < k_file_limit) {
        const Level current = pending[cursor++];
        ++visits;
        std::error_code failure;
        fs::directory_iterator it(
            current.path, fs::directory_options::skip_permission_denied, failure
        );
        if (failure) {
            if (!saw_root) {
                set_error(error, "the folder could not be read");
                return false;
            }
            continue;
        }
        saw_root = true;
        const fs::directory_iterator end;
        while (it != end && files.size() < k_file_limit) {
            const fs::path child = it->path();
            std::error_code status_error;
            const fs::file_status status = it->symlink_status(status_error);
            it.increment(failure);
            if (status_error || fs::is_symlink(status)) {
                if (failure)
                    break;
                continue;
            }
            if (fs::is_directory(status)) {
                if (current.depth + 1 < k_search_depth && !folder_is_not_searched(child))
                    pending.push_back(Level{child, current.depth + 1});
            } else if (fs::is_regular_file(status)) {
                ContentSourceFile listed;
                listed.name = relative_posix(root, child);
                if (!listed.name.empty()) {
                    std::error_code size_error;
                    const auto size = fs::file_size(child, size_error);
                    std::error_code time_error;
                    const auto stamp = fs::last_write_time(child, time_error);
                    if (!size_error) {
                        listed.size = narrow_size(size);
                        if (!time_error)
                            listed.modified = modified_seconds(stamp);
                        files.push_back(std::move(listed));
                    }
                }
            }
            if (failure)
                break;
        }
    }
    return true;
}

[[nodiscard]] bool
resolve_child(const fs::path& directory, std::string_view part, bool as_file, fs::path& resolved) {
    std::error_code failure;
    fs::directory_iterator it(directory, fs::directory_options::skip_permission_denied, failure);
    if (failure)
        return false;
    const fs::directory_iterator end;
    while (it != end) {
        const fs::path child = it->path();
        std::error_code status_error;
        const fs::file_status status = it->symlink_status(status_error);
        it.increment(failure);
        if (status_error || fs::is_symlink(status)) {
            if (failure)
                break;
            continue;
        }
        const bool kind_ok = as_file ? fs::is_regular_file(status) : fs::is_directory(status);
        if (kind_ok && folded_equal(path_to_utf8(child.filename()), part)) {
            resolved = child;
            return true;
        }
        if (failure)
            break;
    }
    return false;
}

enum class PrefixCheck { matched, different, failed };

[[nodiscard]] PrefixCheck match_prefix(
    std::FILE* source,
    std::FILE* destination,
    uint64_t count,
    oa::base::sha256::Hasher& hasher,
    std::string* error
) {
    std::vector<uint8_t> from_source(64 * 1024);
    std::vector<uint8_t> from_dest(from_source.size());
    uint64_t got = 0;
    while (got < count) {
        const uint64_t step = std::min<uint64_t>(from_source.size(), count - got);
        const std::size_t want = static_cast<std::size_t>(step);
        const std::size_t source_read = std::fread(from_source.data(), 1, want, source);
        const std::size_t dest_read = std::fread(from_dest.data(), 1, want, destination);
        if (source_read != want || std::ferror(source) != 0 || std::ferror(destination) != 0) {
            set_error(error, "the file could not be read");
            return PrefixCheck::failed;
        }
        if (dest_read != want || !std::equal(
                                     from_dest.begin(),
                                     from_dest.begin() + static_cast<std::ptrdiff_t>(want),
                                     from_source.begin()
                                 ))
            return PrefixCheck::different;
        oa::base::sha256::update(hasher, std::span<const uint8_t>(from_source.data(), want));
        got += want;
    }
    return PrefixCheck::matched;
}

enum class TailStatus { finished, stopped, no_space, failed };

struct TailRequest {
    std::FILE* source{};
    std::FILE* destination{};
    oa::base::sha256::Hasher* hasher{};
    uint64_t written{};
    uint64_t total{};
    uint64_t chunk{};
    const std::atomic<bool>* stop{};
    void (*on_chunk)(void* context, uint64_t bytes_written){};
    void* chunk_context{};
};

[[nodiscard]] TailStatus write_tail(TailRequest& tail, std::string* error) {
    std::vector<uint8_t> buffer(static_cast<std::size_t>(tail.chunk));
    while (tail.written < tail.total) {
        if (stopped_now(tail.stop)) {
            if (!sync_file(tail.destination)) {
                set_error(error, "the copy could not be stored");
                return TailStatus::failed;
            }
            return TailStatus::stopped;
        }
        const uint64_t step = std::min<uint64_t>(tail.chunk, tail.total - tail.written);
        const std::size_t want = static_cast<std::size_t>(step);
        const std::size_t read = std::fread(buffer.data(), 1, want, tail.source);
        if (read != want) {
            set_error(error, "the file could not be read");
            return TailStatus::failed;
        }
        oa::base::sha256::update(*tail.hasher, std::span<const uint8_t>(buffer.data(), read));
        errno = 0;
        if (std::fwrite(buffer.data(), 1, read, tail.destination) != read) {
            if (disk_is_full(errno)) {
                set_error(error, "the disk filled while the game was copied");
                return TailStatus::no_space;
            }
            set_error(error, "the copy could not be written");
            return TailStatus::failed;
        }
        tail.written += read;
        if (tail.on_chunk != nullptr)
            tail.on_chunk(tail.chunk_context, tail.written);
    }
    if (!sync_file(tail.destination)) {
        if (disk_is_full(errno)) {
            set_error(error, "the disk filled while the game was copied");
            return TailStatus::no_space;
        }
        set_error(error, "the copy could not be stored");
        return TailStatus::failed;
    }
    return TailStatus::finished;
}

[[nodiscard]] uint64_t chunk_size(uint64_t requested) noexcept {
    if (requested == 0 || requested > content_copy_chunk_bytes)
        return content_copy_chunk_bytes;
    return requested;
}

[[nodiscard]] bool digest_file(
    const fs::path& path, uint64_t expected, oa::base::sha256::Digest& digest, std::string* error
) {
    OwnedFile input(oa::platform::open_file(path, "rb"));
    if (input.file == nullptr) {
        set_error(error, "the copied file could not be read");
        return false;
    }
    oa::base::sha256::Hasher hasher;
    std::vector<uint8_t> buffer(64 * 1024);
    uint64_t got = 0;
    while (got < expected) {
        const uint64_t step = std::min<uint64_t>(buffer.size(), expected - got);
        const std::size_t want = static_cast<std::size_t>(step);
        const std::size_t read = std::fread(buffer.data(), 1, want, input.file);
        if (read != want) {
            set_error(error, "the copied file could not be read");
            return false;
        }
        oa::base::sha256::update(hasher, std::span<const uint8_t>(buffer.data(), read));
        got += read;
    }
    digest = oa::base::sha256::finish(hasher);
    return true;
}

/// Hashes every byte of a regular file. A file that cannot be read fails,
/// and the outs are left unset.
///
/// @param path the file
/// @param[out] digest its SHA-256
/// @param[out] size how many bytes were hashed
/// @param[out] modified its modification time, seconds since 1970
/// @param stop when not null and set, hashing stops between chunks
/// @return true when the file was hashed; false when it could not be read or was stopped
[[nodiscard]] bool hash_whole_file(
    const fs::path& path,
    oa::base::sha256::Digest& digest,
    uint64_t& size,
    int64_t& modified,
    const std::atomic<bool>* stop
) {
    std::error_code failure;
    if (!fs::is_regular_file(path, failure))
        return false;
    const auto stamp = fs::last_write_time(path, failure);
    if (failure)
        return false;
    OwnedFile input(oa::platform::open_file(path, "rb"));
    if (input.file == nullptr)
        return false;
    oa::base::sha256::Hasher hasher;
    std::vector<uint8_t> buffer(64 * 1024);
    uint64_t got = 0;
    while (true) {
        if (stop != nullptr && stop->load())
            return false;
        const std::size_t read = std::fread(buffer.data(), 1, buffer.size(), input.file);
        if (read > 0) {
            oa::base::sha256::update(hasher, std::span<const uint8_t>(buffer.data(), read));
            if (got > std::numeric_limits<uint64_t>::max() - static_cast<uint64_t>(read))
                got = std::numeric_limits<uint64_t>::max();
            else
                got += static_cast<uint64_t>(read);
        }
        if (read < buffer.size())
            break;
    }
    if (std::ferror(input.file) != 0)
        return false;
    digest = oa::base::sha256::finish(hasher);
    size = got;
    modified = modified_seconds(stamp);
    return true;
}

[[nodiscard]] std::size_t recorded_index(const ContentRecord& record, std::string_view copy_name) {
    for (std::size_t index = 0; index < record.files.size(); ++index) {
        if (record.files[index].copy_name == copy_name)
            return index;
    }
    return record.files.size();
}

[[nodiscard]] bool set_modified(const fs::path& path, int64_t modified, std::string* error) {
    std::error_code failure;
    fs::last_write_time(path, time_from_seconds(modified), failure);
    if (failure) {
        set_error(error, "the copied file's time could not be set");
        return false;
    }
    return true;
}

[[nodiscard]] bool
file_matches_record(const fs::path& dest, const ContentRecordFile& recorded, uint64_t expected) {
    std::error_code failure;
    if (!fs::is_regular_file(dest, failure))
        return false;
    const auto size = fs::file_size(dest, failure);
    if (failure || narrow_size(size) != expected || recorded.size != expected)
        return false;
    const auto stamp = fs::last_write_time(dest, failure);
    if (failure)
        return false;
    return modified_seconds(stamp) == recorded.modified;
}

[[nodiscard]] uint64_t regular_size(const fs::path& path) {
    std::error_code failure;
    if (!fs::is_regular_file(path, failure))
        return 0;
    const auto size = fs::file_size(path, failure);
    if (failure)
        return 0;
    return narrow_size(size);
}

void remove_file(const fs::path& path) {
    std::error_code ignored;
    fs::remove(path, ignored);
}

[[nodiscard]] fs::path stored_path(const fs::path& folder, std::string_view copy_name) {
    fs::path dest = folder;
    std::string part;
    for (char byte : copy_name) {
        if (byte == '/' || byte == '\\') {
            if (!part.empty()) {
                dest /= path_from_utf8(part);
                part.clear();
            }
        } else {
            part.push_back(byte);
        }
    }
    if (!part.empty())
        dest /= path_from_utf8(part);
    return dest;
}

[[nodiscard]] fs::path partial_path(const fs::path& dest) {
    fs::path partial = dest;
    partial += ".part";
    return partial;
}

void notify(const CopyRequest& request, const CopyProgress& progress) {
    if (request.on_progress != nullptr)
        request.on_progress(request.progress_context, progress);
}

struct ChunkProgress {
    const CopyRequest* request{};
    CopyProgress* progress{};
    uint64_t done_before{};
};

void note_chunk(void* context, uint64_t bytes_written) {
    auto* note = static_cast<ChunkProgress*>(context);
    note->progress->bytes_done = note->done_before + bytes_written;
    note->progress->file_finished = false;
    notify(*note->request, *note->progress);
}

[[nodiscard]] FreeSpace query_free(const CopyRequest& request, const fs::path& directory) {
    const fs::path existing = existing_ancestor(directory);
    if (request.free_space != nullptr)
        return request.free_space(request.free_space_context, existing);
    FreeSpace report;
    if (existing.empty())
        return report;
    try {
        const fs::space_info info = fs::space(existing);
        report.bytes = narrow_size(info.available);
        report.known = true;
    } catch (const std::exception&) {
        report.known = false;
    }
    return report;
}

[[nodiscard]] const ContentRecordFile*
find_recorded(const ContentRecord& record, std::string_view copy_name) noexcept {
    for (const ContentRecordFile& item : record.files) {
        if (item.copy_name == copy_name)
            return &item;
    }
    return nullptr;
}

struct PlannedFile {
    MatchedContentFile matched;
    fs::path dest;
    fs::path partial;
    uint64_t prefix{};
    bool complete{};
    bool shareable{};
};

[[nodiscard]] bool duplicate_file(const fs::path& from, const fs::path& to, std::string* error) {
    remove_file(to);
    std::string share_error;
    if (oa::platform::blocks_can_be_shared(from, to.parent_path()) &&
        oa::platform::share_file_blocks(from, to, &share_error))
        return true;
    std::error_code failure;
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, failure);
    if (failure) {
        set_error(error, "a file could not be copied");
        return false;
    }
    return true;
}

[[nodiscard]] bool remember_file(
    ContentRecord& record, const PlannedFile& planned, const oa::base::sha256::Digest& digest
) {
    ContentRecordFile stored;
    stored.copy_name = planned.matched.copy_name;
    stored.size = planned.matched.size;
    stored.modified = planned.matched.modified;
    stored.digest = digest;
    bool replaced = false;
    for (ContentRecordFile& item : record.files) {
        if (item.copy_name == stored.copy_name) {
            item = stored;
            replaced = true;
            break;
        }
    }
    if (!replaced)
        record.files.push_back(std::move(stored));
    record.bundle_id = planned.matched.bundle_id;
    record.present = true;
    record.valid = true;
    return true;
}

void say_line(std::ostream* news, const char* line) {
    if (news != nullptr)
        *news << "open-annihilation: " << line << '\n';
}

constexpr std::string_view k_rev_bytes = "rev31-fixture\n";
constexpr std::string_view k_zrb_bytes = "zrb-fixture\n";
constexpr std::string_view k_campaign_bytes = "campaign-fixture\n";

[[nodiscard]] bool load_bytes(const fs::path& path, std::string& bytes, std::string& reason) {
    OwnedFile input(oa::platform::open_file(path, "rb"));
    if (input.file == nullptr) {
        reason = "a copied file could not be read";
        return false;
    }
    bytes.clear();
    char buffer[4096];
    while (true) {
        const std::size_t read = std::fread(buffer, 1, sizeof buffer, input.file);
        if (read > 0)
            bytes.append(buffer, read);
        if (read < sizeof buffer)
            break;
    }
    if (std::ferror(input.file) != 0) {
        reason = "a copied file could not be read";
        return false;
    }
    return true;
}

struct SourceStamp {
    fs::path path;
    uint64_t size{};
    int64_t modified{};
    std::string bytes;
};

[[nodiscard]] bool
stamp_tree(const fs::path& root, std::vector<SourceStamp>& stamps, std::string& reason) {
    stamps.clear();
    std::error_code failure;
    fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, failure);
    if (failure) {
        reason = "the fixture could not be read";
        return false;
    }
    // The fixture is shallow: one directory of archives and one Data folder.
    std::vector<fs::path> pending;
    pending.push_back(root);
    std::size_t cursor = 0;
    while (cursor < pending.size()) {
        const fs::path directory = pending[cursor++];
        fs::directory_iterator cursor_it(
            directory, fs::directory_options::skip_permission_denied, failure
        );
        if (failure)
            continue;
        const fs::directory_iterator end;
        while (cursor_it != end) {
            const fs::path child = cursor_it->path();
            std::error_code status_error;
            const fs::file_status status = cursor_it->symlink_status(status_error);
            cursor_it.increment(failure);
            if (status_error || fs::is_symlink(status)) {
                if (failure)
                    break;
                continue;
            }
            if (fs::is_directory(status)) {
                pending.push_back(child);
            } else if (fs::is_regular_file(status)) {
                SourceStamp stamp;
                stamp.path = child;
                stamp.size = regular_size(child);
                const auto written = fs::last_write_time(child, status_error);
                if (status_error) {
                    reason = "the fixture's time could not be read";
                    return false;
                }
                stamp.modified = modified_seconds(written);
                if (!load_bytes(child, stamp.bytes, reason))
                    return false;
                stamps.push_back(std::move(stamp));
            }
            if (failure)
                break;
        }
    }
    return true;
}

[[nodiscard]] bool stamps_unchanged(const std::vector<SourceStamp>& stamps, std::string& reason) {
    for (const SourceStamp& stamp : stamps) {
        if (regular_size(stamp.path) != stamp.size) {
            reason = "the source changed size";
            return false;
        }
        std::error_code failure;
        const auto written = fs::last_write_time(stamp.path, failure);
        if (failure || modified_seconds(written) != stamp.modified) {
            reason = "the source changed time";
            return false;
        }
        std::string bytes;
        if (!load_bytes(stamp.path, bytes, reason))
            return false;
        if (bytes != stamp.bytes) {
            reason = "the source changed bytes";
            return false;
        }
    }
    return true;
}

struct StopAfterFile {
    std::atomic<bool>* flag{};
    int finished{};
};

void stop_after_one_file(void* context, const CopyProgress& progress) {
    auto* state = static_cast<StopAfterFile*>(context);
    if (state->finished > 0 || !progress.file_finished)
        return;
    state->finished = 1;
    state->flag->store(true);
}

[[nodiscard]] bool write_fixture_file(
    const fs::path& install, std::string_view copy_name, std::string_view bytes, std::string& reason
) {
    const fs::path dest = stored_path(install, copy_name);
    std::error_code failure;
    fs::create_directories(dest.parent_path(), failure);
    if (failure) {
        reason = "the fixture could not be written";
        return false;
    }
    const auto* data = reinterpret_cast<const uint8_t*>(bytes.data());
    if (!oa::platform::replace_file(dest, std::span<const uint8_t>(data, bytes.size()), &reason)) {
        if (reason.empty())
            reason = "the fixture could not be written";
        return false;
    }
    fs::last_write_time(dest, time_from_seconds(k_check_time), failure);
    if (failure) {
        reason = "the fixture's time could not be set";
        return false;
    }
    return true;
}

// A finished copy leaves no file whose name ends in .part.
[[nodiscard]] bool partial_left(const fs::path& root) {
    std::error_code failure;
    if (!fs::exists(root, failure))
        return false;
    fs::recursive_directory_iterator cursor(
        root, fs::directory_options::skip_permission_denied, failure
    );
    if (failure)
        return true;
    const fs::recursive_directory_iterator end;
    while (cursor != end) {
        const fs::path child = cursor->path();
        cursor.increment(failure);
        if (child.extension() == ".part")
            return true;
        if (failure)
            return true;
    }
    return false;
}

/// A folder whose name cannot hold the game is not walked while a root is sought.
///
/// @param folder the folder
/// @return true when a search leaves it unopened
[[nodiscard]] bool folder_is_not_searched(const fs::path& folder) {
    return oa::platform::game_installs::folder_cannot_hold_game(path_to_utf8(folder.filename()));
}

/// Bytes hashed from each end of a published installer. A file shorter than
/// this contributes only its own bytes, once.
constexpr uint64_t k_installer_end_bytes = uint64_t{1024} * 1024;

/// One published installer the setup can name without reading the archive.
/// The digest is the SHA-256 of the first megabyte followed by the last.
struct PublishedListing {
    uint64_t size{};
    oa::base::sha256::Digest ends{};
    std::span<const std::string_view> absent{};
};

/// Copy names the Windows Commander Pack installer does not hold.
constexpr std::string_view k_windows_installer_absent[]{"TADemo.hpi"};
/// Copy names the macOS Commander Pack installer does not hold.
constexpr std::string_view k_macos_installer_absent[]{
    "TADemo.hpi",
    "music/0.mp3",
    "music/13.mp3",
    "afark.ufo",
    "aflea.ufo",
    "ascarab.ufo",
    "cormabm.ufo",
    "cornecro.ufo",
    "corplas.ufo",
};
/// Copy names the older-Mac Commander Pack disk image does not hold.
constexpr std::string_view k_older_mac_installer_absent[]{
    "TADemo.hpi",
    "afark.ufo",
    "aflea.ufo",
    "ascarab.ufo",
    "cormabm.ufo",
    "cornecro.ufo",
    "corplas.ufo",
};

/// The three published installers, keyed by the size in the known-installer table.
constexpr PublishedListing k_published_listings[]{
    PublishedListing{
        1093885504,
        oa::base::sha256::parse_hex(
            "287ea8b5533b405f929299dbcb381e688af5cd16613f65f388b1363badd00d92"
        )
            .value(),
        k_windows_installer_absent,
    },
    PublishedListing{
        2559350261,
        oa::base::sha256::parse_hex(
            "a539de6828a04e0b6458787be7b646d23293c39eb31162004179189e0aad5656"
        )
            .value(),
        k_macos_installer_absent,
    },
    PublishedListing{
        1144736288,
        oa::base::sha256::parse_hex(
            "ec4b57fb6079b349460cde5e52a7b6d485808bc8d94cd62f06ea2eba1ed3b19c"
        )
            .value(),
        k_older_mac_installer_absent,
    },
};

/// Finds the listing of an installer size.
///
/// @param size the file's size
/// @return the listing, or null
[[nodiscard]] const PublishedListing* published_listing(uint64_t size) noexcept {
    for (const PublishedListing& listing : k_published_listings) {
        if (listing.size == size)
            return &listing;
    }
    return nullptr;
}

/// Moves a stream to a byte offset from the start.
///
/// @param file the stream
/// @param offset the offset
/// @return true when the stream is there
[[nodiscard]] bool seek_set(std::FILE* file, uint64_t offset) noexcept {
#if defined(_WIN32)
    return _fseeki64(file, static_cast<__int64>(offset), SEEK_SET) == 0;
#else
    if (offset > static_cast<uint64_t>(std::numeric_limits<off_t>::max()))
        return false;
    return ::fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

/// Reads a count of bytes, or fails when the stream ends first.
///
/// @param file the stream
/// @param data where the bytes are stored
/// @param count how many
/// @return true when every byte was read
[[nodiscard]] bool read_exact(std::FILE* file, uint8_t* data, std::size_t count) noexcept {
    std::size_t got = 0;
    while (got < count) {
        const std::size_t piece = std::fread(data + got, 1, count - got, file);
        if (piece == 0)
            return false;
        got += piece;
    }
    return true;
}

/// Hashes the first megabyte of a file and, when the file is longer, the last.
///
/// @param file the file
/// @param size its size
/// @param[out] digest the hash
/// @param[out] error why the file could not be read; may be null
/// @return true when `digest` is that hash
[[nodiscard]] bool hash_installer_ends(
    const fs::path& file, uint64_t size, oa::base::sha256::Digest& digest, std::string* error
) {
    OwnedFile opened(oa::platform::open_file(file, "rb"));
    if (opened.file == nullptr) {
        set_error(error, "the file could not be read");
        return false;
    }
    const uint64_t head = std::min(size, k_installer_end_bytes);
    const uint64_t tail = size > k_installer_end_bytes ? std::min(size, k_installer_end_bytes) : 0;
    const uint64_t longest = std::max(head, tail);
    std::vector<uint8_t> buffer(static_cast<std::size_t>(longest));
    if (head > 0 && !read_exact(opened.file, buffer.data(), static_cast<std::size_t>(head))) {
        set_error(error, "the file could not be read");
        return false;
    }
    oa::base::sha256::Hasher hasher;
    oa::base::sha256::update(
        hasher, std::span<const uint8_t>(buffer.data(), static_cast<std::size_t>(head))
    );
    if (tail > 0) {
        if (!seek_set(opened.file, size - tail) ||
            !read_exact(opened.file, buffer.data(), static_cast<std::size_t>(tail))) {
            set_error(error, "the file could not be read");
            return false;
        }
        oa::base::sha256::update(
            hasher, std::span<const uint8_t>(buffer.data(), static_cast<std::size_t>(tail))
        );
    }
    digest = oa::base::sha256::finish(hasher);
    return true;
}

/// Tells whether a copy name is one the installer does not hold.
///
/// @param absent the names
/// @param copy_name the bundle's copy name
/// @return true when the installer does not hold it
[[nodiscard]] bool
copy_name_absent(std::span<const std::string_view> absent, std::string_view copy_name) noexcept {
    for (const std::string_view name : absent) {
        if (folded_equal(name, copy_name))
            return true;
    }
    return false;
}

} // namespace

std::string_view unattended_copy_refusal() noexcept {
    return "Open Annihilation has no copy of the game in its own folder, and this run does not "
           "make one. Start the game so it can copy the files, or name a folder with --game-dir "
           "PATH.";
}

bool archive_copy_name(std::string_view copy_name) noexcept {
    constexpr std::string_view suffixes[] = {".hpi", ".ufo", ".ccx", ".gp3"};
    for (const std::string_view suffix : suffixes) {
        if (copy_name.size() > suffix.size() &&
            folded_equal(copy_name.substr(copy_name.size() - suffix.size()), suffix))
            return true;
    }
    return false;
}

FolderContentSource::FolderContentSource(fs::path folder, std::span<const bundles::Bundle> table)
    : folder_(std::move(folder)), bundles_(table) {
}

std::string FolderContentSource::origin() const {
    return path_to_utf8(folder_);
}

bool FolderContentSource::open(std::string* error) {
    if (opened_)
        return true;
    std::error_code failure;
    if (!fs::is_directory(folder_, failure)) {
        set_error(error, "the folder could not be read");
        return false;
    }
    root_ = find_game_root(folder_, bundles_);
    opened_ = true;
    return true;
}

bool FolderContentSource::list(std::vector<ContentSourceFile>& files, std::string* error) {
    if (!open(error))
        return false;
    return list_tree(root_, files, error);
}

bool FolderContentSource::locate(std::string_view name, fs::path& resolved, std::string* error) {
    if (!open(error))
        return false;
    std::vector<std::string> parts;
    if (!split_relative(name, parts)) {
        set_error(error, "the file could not be found");
        return false;
    }
    fs::path current = root_;
    for (std::size_t index = 0; index < parts.size(); ++index) {
        fs::path next;
        const bool last = index + 1 == parts.size();
        if (!resolve_child(current, parts[index], last, next)) {
            set_error(error, "the file could not be found");
            return false;
        }
        current = std::move(next);
    }
    resolved = std::move(current);
    return true;
}

fs::path FolderContentSource::disk_root() const {
    return opened_ ? root_ : folder_;
}

bool FolderContentSource::stream_file(
    const StreamRequest& request, StreamResult& result, std::string* error
) {
    result = {};
    fs::path source_path;
    if (!locate(request.name, source_path, error))
        return false;
    std::error_code failure;
    const auto source_size_raw = fs::file_size(source_path, failure);
    if (failure || !fs::is_regular_file(source_path, failure)) {
        set_error(error, "the file could not be read");
        return false;
    }
    const uint64_t source_size = narrow_size(source_size_raw);
    uint64_t prefix = regular_size(request.destination);
    if (prefix > source_size) {
        remove_file(request.destination);
        prefix = 0;
    }
    if (stopped_now(request.stop) && prefix == 0) {
        result.stopped = true;
        return true;
    }
    const uint64_t chunk = chunk_size(request.chunk_bytes);
    oa::base::sha256::Hasher hasher;
    OwnedFile source(oa::platform::open_file(source_path, "rb"));
    if (source.file == nullptr) {
        set_error(error, "the file could not be read");
        return false;
    }
    if (prefix > 0) {
        PrefixCheck compared = PrefixCheck::failed;
        {
            // The copy so far is closed before it is removed or appended to.
            OwnedFile existing(oa::platform::open_file(request.destination, "rb"));
            if (existing.file == nullptr) {
                set_error(error, "the copy could not be read");
                return false;
            }
            compared = match_prefix(source.file, existing.file, prefix, hasher, error);
        }
        if (compared == PrefixCheck::failed)
            return false;
        if (compared == PrefixCheck::different) {
            remove_file(request.destination);
            prefix = 0;
            hasher = oa::base::sha256::Hasher{};
            if (std::fseek(source.file, 0, SEEK_SET) != 0) {
                set_error(error, "the file could not be read");
                return false;
            }
        } else if (prefix == source_size) {
            result.digest = oa::base::sha256::finish(hasher);
            result.bytes = source_size;
            return true;
        }
    }
    const char* mode = prefix == 0 ? "wb" : "ab";
    OwnedFile destination(oa::platform::open_file(request.destination, mode));
    if (destination.file == nullptr) {
        set_error(error, "the copy could not be written");
        return false;
    }
    TailRequest tail;
    tail.source = source.file;
    tail.destination = destination.file;
    tail.hasher = &hasher;
    tail.written = prefix;
    tail.total = source_size;
    tail.chunk = chunk;
    tail.stop = request.stop;
    tail.on_chunk = request.on_chunk;
    tail.chunk_context = request.chunk_context;
    const TailStatus status = write_tail(tail, error);
    result.bytes = tail.written;
    if (status == TailStatus::stopped) {
        result.stopped = true;
        return true;
    }
    if (status == TailStatus::no_space) {
        result.no_space = true;
        return false;
    }
    if (status != TailStatus::finished)
        return false;
    result.digest = oa::base::sha256::finish(hasher);
    result.bytes = source_size;
    return true;
}

ContentRecord read_content_record(const fs::path& file, std::string* error) {
    return oa::data::content_bundles::read_bundle_folder_record(file.parent_path(), error);
}

bool write_content_record(const fs::path& file, const ContentRecord& record, std::string* error) {
    if (record.files.empty())
        return true;
    const std::optional<std::string> text =
        oa::data::content_bundles::format_content_record(record);
    if (!text) {
        set_error(error, "the record could not be written");
        return false;
    }
    const auto* bytes = reinterpret_cast<const uint8_t*>(text->data());
    if (!oa::platform::replace_file(file, std::span<const uint8_t>(bytes, text->size()), error))
        return false;
    note_content_bundles_changed();
    return true;
}

fs::path bundle_folder(const fs::path& user_folder, const bundles::Bundle& bundle) {
    const fs::path id{std::string(bundle.id)};
    if (bundle.kind == bundles::Kind::base_game)
        return user_folder / std::string(managed_games_folder) / id;
    if (bundle.extends.empty())
        return user_folder / std::string(managed_expansions_folder) / id;
    return user_folder / std::string(managed_expansions_folder) / std::string(bundle.extends[0]) /
           id;
}

bool describe_source(
    ContentSource& source,
    std::span<const bundles::Bundle> table,
    SourceDescription& described,
    std::string* error
) {
    described = {};
    std::vector<ContentSourceFile> listed;
    if (!source.list(listed, error))
        return false;
    std::vector<char> used(listed.size(), 0);
    for (const bundles::Bundle& bundle : table) {
        bool held = false;
        for (const bundles::ContentFile& file : bundle.files) {
            bool found = false;
            for (std::size_t index = 0; index < listed.size(); ++index) {
                if (used[index] != 0 || listed[index].size != file.size)
                    continue;
                if (!names_match(listed[index].name, file))
                    continue;
                used[index] = 1;
                MatchedContentFile matched;
                matched.bundle_id = std::string(bundle.id);
                matched.copy_name = std::string(file.copy_name);
                matched.source_name = listed[index].name;
                matched.size = listed[index].size;
                matched.modified = listed[index].modified;
                add_bytes(described.bytes_to_copy, matched.size);
                described.matched.push_back(std::move(matched));
                held = true;
                found = true;
                break;
            }
            if (!found) {
                MissingContentFile missing;
                missing.bundle_id = std::string(bundle.id);
                missing.copy_name = std::string(file.copy_name);
                for (std::size_t index = 0; index < listed.size(); ++index) {
                    if (used[index] != 0 || !names_match(listed[index].name, file))
                        continue;
                    if (listed[index].size != file.size) {
                        missing.different_size = true;
                        missing.source_size = listed[index].size;
                    }
                }
                described.missing.push_back(std::move(missing));
            }
        }
        if (held)
            described.bundles.push_back(std::string(bundle.id));
    }
    return true;
}

bool is_published_installer_name(const fs::path& file, const bundles::Installer& installer) {
    return folded_equal(path_to_utf8(file.filename()), installer.file_name);
}

std::span<const std::string_view> installer_not_held(const bundles::Installer& installer) noexcept {
    const PublishedListing* listing = published_listing(installer.size);
    if (listing == nullptr)
        return {};
    return listing->absent;
}

bool describe_published_holdings(
    const bundles::Installer& installer, SourceDescription& described
) {
    described = {};
    const PublishedListing* listing = published_listing(installer.size);
    if (listing == nullptr)
        return false;
    for (const bundles::Bundle& bundle : bundles::bundles()) {
        bool held = false;
        for (const bundles::ContentFile& file : bundle.files) {
            if (copy_name_absent(listing->absent, file.copy_name)) {
                MissingContentFile missing;
                missing.bundle_id = std::string(bundle.id);
                missing.copy_name = std::string(file.copy_name);
                described.missing.push_back(std::move(missing));
                continue;
            }
            MatchedContentFile matched;
            matched.bundle_id = std::string(bundle.id);
            matched.copy_name = std::string(file.copy_name);
            matched.source_name = matched.copy_name;
            matched.size = file.size;
            add_bytes(described.bytes_to_copy, matched.size);
            described.matched.push_back(std::move(matched));
            held = true;
        }
        if (held)
            described.bundles.push_back(std::string(bundle.id));
    }
    return true;
}

bool describe_published_installer(
    const fs::path& file, SourceDescription& described, std::string* error
) {
    described = {};
    std::error_code failure;
    if (!fs::is_regular_file(file, failure) || failure) {
        set_error(error, "the file could not be read");
        return false;
    }
    const auto raw_size = fs::file_size(file, failure);
    if (failure) {
        set_error(error, "the file could not be read");
        return false;
    }
    const uint64_t size = narrow_size(raw_size);
    const bundles::Installer* installer = bundles::find_installer(size);
    if (installer == nullptr || !is_published_installer_name(file, *installer)) {
        set_error(error, "the file is not a known installer");
        return false;
    }
    const PublishedListing* listing = published_listing(size);
    if (listing == nullptr) {
        set_error(error, "the file is not a known installer");
        return false;
    }
    oa::base::sha256::Digest ends;
    if (!hash_installer_ends(file, size, ends, error))
        return false;
    if (ends != listing->ends) {
        set_error(error, "the file is not a known installer");
        return false;
    }
    return describe_published_holdings(*installer, described);
}

bool managed_base_present(const fs::path& user_folder, std::span<const bundles::Bundle> table) {
    for (const bundles::Bundle& bundle : table) {
        if (bundle.kind != bundles::Kind::base_game)
            continue;
        const fs::path record_path =
            bundle_folder(user_folder, bundle) / std::string(content_record_name);
        const ContentRecord record = read_content_record(record_path, nullptr);
        if (bundles::record_complete(bundle, record))
            return true;
    }
    return false;
}

CopyResult copy_content(ContentSource& source, const CopyRequest& request) {
    CopyResult together;
    if (source.copy_together(request, together))
        return together;
    CopyResult result;
    try {
        SourceDescription described;
        std::string problem;
        if (!describe_source(source, request.bundles, described, &problem)) {
            result.status = CopyStatus::failed;
            result.error = player_error(problem.empty() ? "the folder could not be read" : problem);
            return result;
        }
        // An archive of a base game or of an expansion at another size: the
        // place was changed, by a mod or another program. Nothing is copied
        // or recorded, so no half-done copy is taken as made. Such a file is
        // one of its bundle's files held under its name, so the bundle is
        // there; a bundle the place holds none of has nothing to stop on.
        for (const MissingContentFile& missing : described.missing) {
            if (!missing.different_size || !archive_copy_name(missing.copy_name))
                continue;
            for (const bundles::Bundle& bundle : request.bundles) {
                if (bundle.id != missing.bundle_id)
                    continue;
                for (const bundles::ContentFile& file : bundle.files) {
                    if (file.copy_name != missing.copy_name)
                        continue;
                    ChangedContentFile changed;
                    changed.bundle_id = missing.bundle_id;
                    changed.copy_name = missing.copy_name;
                    changed.source_size = missing.source_size;
                    changed.size = file.size;
                    result.changed.push_back(std::move(changed));
                    const std::string line = missing.copy_name + " of " +
                                             std::string(bundle.english_name) +
                                             " is not copied: the source holds it at " +
                                             std::to_string(missing.source_size) + " bytes, not " +
                                             std::to_string(file.size);
                    say_line(request.news, line.c_str());
                }
            }
        }
        if (!result.changed.empty()) {
            std::string sizes;
            for (const ChangedContentFile& changed : result.changed) {
                sizes += sizes.empty() ? "" : "; ";
                sizes += changed.copy_name + " is " + std::to_string(changed.source_size) +
                         " bytes, not " + std::to_string(changed.size);
            }
            result.status = CopyStatus::changed;
            result.error = "This Total Annihilation has been changed, as a mod or another program "
                           "changes it (" +
                           sizes +
                           "). Open Annihilation needs a fresh Total Annihilation install, "
                           "without mods, to set up its copy. Nothing was copied.";
            say_line(request.news, "Nothing was copied: this Total Annihilation has been changed.");
            return result;
        }
        if (described.matched.empty())
            return result;
        const fs::path origin = path_from_utf8(source.origin());
        const fs::path root = source.disk_root();
        const fs::path games = request.user_folder / std::string(managed_games_folder);
        const fs::path expansions = request.user_folder / std::string(managed_expansions_folder);
        if (path_is_inside(origin, games) || path_is_inside(origin, expansions) ||
            path_is_inside(root, games) || path_is_inside(root, expansions) ||
            path_is_inside(request.user_folder, origin) ||
            path_is_inside(request.user_folder, root))
            return result;

        std::vector<PlannedFile> planned;
        planned.reserve(described.matched.size());
        std::string loaded_id;
        ContentRecord loaded;
        bool loaded_ok = false;
        for (MatchedContentFile& matched : described.matched) {
            if (!safe_copy_name(matched.copy_name)) {
                result.status = CopyStatus::failed;
                result.error = player_error("a file name cannot be stored");
                return result;
            }
            const bundles::Bundle* bundle = nullptr;
            for (const bundles::Bundle& candidate : request.bundles) {
                if (candidate.id == matched.bundle_id) {
                    bundle = &candidate;
                    break;
                }
            }
            if (bundle == nullptr) {
                result.status = CopyStatus::failed;
                result.error = player_error("a bundle could not be found");
                return result;
            }
            if (loaded_id != matched.bundle_id) {
                const fs::path record_path =
                    bundle_folder(request.user_folder, *bundle) / std::string(content_record_name);
                loaded = read_content_record(record_path, &problem);
                if (loaded.io_error) {
                    result.status = CopyStatus::failed;
                    result.error =
                        player_error(problem.empty() ? "the record could not be read" : problem);
                    return result;
                }
                loaded_ok = loaded.valid && loaded.bundle_id == matched.bundle_id;
                loaded_id = matched.bundle_id;
                if (!loaded_ok)
                    loaded.files.clear();
            }
            PlannedFile item;
            item.dest = stored_path(bundle_folder(request.user_folder, *bundle), matched.copy_name);
            item.partial = partial_path(item.dest);
            const ContentRecordFile* recorded =
                loaded_ok ? find_recorded(loaded, matched.copy_name) : nullptr;
            item.complete =
                recorded != nullptr && file_matches_record(item.dest, *recorded, matched.size);
            item.prefix = item.complete ? 0 : regular_size(item.partial);
            if (item.prefix > matched.size) {
                remove_file(item.partial);
                item.prefix = 0;
            }
            item.shareable = request.share_blocks && item.prefix == 0 && !item.complete;
            item.matched = std::move(matched);
            planned.push_back(std::move(item));
        }

        fs::path sample_file;
        bool volume_shares = false;
        if (request.share_blocks &&
            source.locate(planned.front().matched.source_name, sample_file, nullptr)) {
            const fs::path space_dir = existing_ancestor(request.user_folder);
            if (!space_dir.empty())
                volume_shares = oa::platform::blocks_can_be_shared(sample_file, space_dir);
        }
        uint64_t byte_bytes = 0;
        bool any_left = false;
        for (const PlannedFile& item : planned) {
            if (item.complete)
                continue;
            any_left = true;
            const bool will_share = volume_shares && item.shareable;
            if (!will_share)
                add_bytes(byte_bytes, item.matched.size - item.prefix);
        }
        if (any_left) {
            const bool only_shared = volume_shares && byte_bytes == 0;
            const uint64_t need = bytes_to_make_room_for(byte_bytes, only_shared);
            const FreeSpace free = query_free(request, request.user_folder);
            if (!room_is_enough(free.bytes, free.known, need)) {
                result.status = CopyStatus::no_space;
                result.error = "There is not enough free space to copy the game into Open "
                               "Annihilation. Free some space and start the game again.";
                return result;
            }
        }

        CopyProgress progress;
        progress.bytes_total = described.bytes_to_copy;
        uint32_t table_files = 0;
        for (const bundles::Bundle& bundle : request.bundles) {
            const uint32_t count = narrow_count(bundle.files.size());
            if (table_files > std::numeric_limits<uint32_t>::max() - count)
                table_files = std::numeric_limits<uint32_t>::max();
            else
                table_files += count;
        }
        const uint32_t taken = narrow_count(planned.size());
        const uint32_t not_taken = table_files >= taken ? table_files - taken : 0;
        // The overall count is the files this copy takes, not every table file.
        progress.files_total = copy_file_count(taken, table_files, not_taken).total;
        uint64_t done_bytes = 0;
        bool told = false;
        loaded_id.clear();
        loaded = {};
        loaded_ok = false;
        for (PlannedFile& item : planned) {
            progress.bundle_id = item.matched.bundle_id;
            progress.copy_name = item.matched.copy_name;
            progress.file_finished = false;
            if (item.complete) {
                add_bytes(done_bytes, item.matched.size);
                progress.bytes_done = done_bytes;
                progress.files_done =
                    narrow_count(static_cast<std::size_t>(progress.files_done) + 1);
                progress.file_finished = true;
                notify(request, progress);
                result.files_skipped =
                    narrow_count(static_cast<std::size_t>(result.files_skipped) + 1);
                remove_file(item.partial);
                continue;
            }
            if (stopped_now(request.stop)) {
                result.status = CopyStatus::stopped;
                result.error = "The copy into Open Annihilation stopped. Start the game again to "
                               "continue it.";
                return result;
            }
            const bundles::Bundle* bundle = nullptr;
            for (const bundles::Bundle& candidate : request.bundles) {
                if (candidate.id == item.matched.bundle_id) {
                    bundle = &candidate;
                    break;
                }
            }
            if (bundle == nullptr) {
                result.status = CopyStatus::failed;
                result.error = player_error("a bundle could not be found");
                return result;
            }
            if (loaded_id != item.matched.bundle_id) {
                const fs::path record_path =
                    bundle_folder(request.user_folder, *bundle) / std::string(content_record_name);
                loaded = read_content_record(record_path, &problem);
                loaded_ok = loaded.valid && loaded.bundle_id == item.matched.bundle_id;
                if (!loaded_ok)
                    loaded.files.clear();
                loaded.source = source.origin();
                loaded_id = item.matched.bundle_id;
            }
            std::error_code failure;
            fs::create_directories(item.dest.parent_path(), failure);
            if (failure) {
                result.status = CopyStatus::failed;
                result.error = player_error("the folder could not be created");
                return result;
            }
            if (!told) {
                say_line(request.news, "Copying the game into Open Annihilation...");
                told = true;
            }
            remove_file(item.dest);
            oa::base::sha256::Digest digest{};
            bool stored = false;
            bool cloned = false;
            if (volume_shares && item.shareable) {
                fs::path resolved;
                std::string share_error;
                if (source.locate(item.matched.source_name, resolved, nullptr) &&
                    oa::platform::share_file_blocks(resolved, item.dest, &share_error)) {
                    std::error_code size_error;
                    const auto shared_size = fs::file_size(item.dest, size_error);
                    if (!size_error && narrow_size(shared_size) == item.matched.size &&
                        digest_file(item.dest, item.matched.size, digest, &problem) &&
                        set_modified(item.dest, item.matched.modified, &problem)) {
                        stored = true;
                        cloned = true;
                    } else {
                        remove_file(item.dest);
                    }
                } else {
                    remove_file(item.dest);
                }
            }
            if (!stored) {
                const uint64_t remainder = item.matched.size - item.prefix;
                const uint64_t file_need = bytes_to_make_room_for(remainder, false);
                const FreeSpace free_now = query_free(request, item.dest.parent_path());
                if (!room_is_enough(free_now.bytes, free_now.known, file_need)) {
                    result.status = CopyStatus::no_space;
                    result.error = "There is not enough free space to copy the game into Open "
                                   "Annihilation. Free some space and start the game again.";
                    return result;
                }
                ChunkProgress chunk_note;
                chunk_note.request = &request;
                chunk_note.progress = &progress;
                chunk_note.done_before = done_bytes;
                StreamRequest stream;
                stream.name = item.matched.source_name;
                stream.destination = item.partial;
                stream.chunk_bytes = request.chunk_bytes;
                stream.stop = request.stop;
                stream.on_chunk = note_chunk;
                stream.chunk_context = &chunk_note;
                StreamResult streamed;
                if (!source.stream_file(stream, streamed, &problem)) {
                    result.status = streamed.no_space ? CopyStatus::no_space : CopyStatus::failed;
                    result.error =
                        streamed.no_space
                            ? std::string(
                                  "There is not enough free space to copy the "
                                  "game into Open Annihilation. Free some space "
                                  "and start the game again."
                              )
                            : player_error(
                                  problem.empty() ? "a file could not be copied" : problem
                              );
                    return result;
                }
                if (streamed.stopped) {
                    result.status = CopyStatus::stopped;
                    result.error = "The copy into Open Annihilation stopped. Start the game again "
                                   "to continue it.";
                    return result;
                }
                if (!set_modified(item.partial, item.matched.modified, &problem)) {
                    result.status = CopyStatus::failed;
                    result.error = player_error(problem);
                    return result;
                }
                fs::rename(item.partial, item.dest, failure);
                if (failure) {
                    result.status = CopyStatus::failed;
                    result.error = player_error("a copied file could not be stored");
                    return result;
                }
                if (!set_modified(item.dest, item.matched.modified, &problem)) {
                    result.status = CopyStatus::failed;
                    result.error = player_error(problem);
                    return result;
                }
                digest = streamed.digest;
                stored = true;
            }
            if (!stored) {
                result.status = CopyStatus::failed;
                result.error = player_error("a file could not be copied");
                return result;
            }
            loaded.source = source.origin();
            if (!remember_file(loaded, item, digest)) {
                result.status = CopyStatus::failed;
                result.error = player_error("the record could not be updated");
                return result;
            }
            const fs::path record_path =
                bundle_folder(request.user_folder, *bundle) / std::string(content_record_name);
            if (!write_content_record(record_path, loaded, &problem)) {
                result.status = CopyStatus::failed;
                result.error =
                    player_error(problem.empty() ? "the record could not be written" : problem);
                return result;
            }
            remove_file(item.partial);
            add_bytes(done_bytes, item.matched.size);
            const uint64_t fresh = cloned ? item.matched.size : item.matched.size - item.prefix;
            add_bytes(result.bytes_copied, fresh);
            result.files_copied = narrow_count(static_cast<std::size_t>(result.files_copied) + 1);
            progress.bytes_done = done_bytes;
            progress.files_done = narrow_count(static_cast<std::size_t>(progress.files_done) + 1);
            progress.file_finished = true;
            notify(request, progress);
        }
        // A table file the source did not name can still be the bytes of
        // another file in that bundle, when the size matches and the hash is
        // one the table accepts for the missing name. Anything left over is
        // recorded as not in the source, and no file is written for it.
        for (const MissingContentFile& missing : described.missing) {
            bool held = false;
            for (const std::string& id : described.bundles) {
                if (id == missing.bundle_id) {
                    held = true;
                    break;
                }
            }
            if (!held)
                continue;
            const bundles::Bundle* bundle = nullptr;
            const bundles::ContentFile* table_file = nullptr;
            for (const bundles::Bundle& candidate : request.bundles) {
                if (candidate.id != missing.bundle_id)
                    continue;
                bundle = &candidate;
                for (const bundles::ContentFile& file : candidate.files) {
                    if (file.copy_name == missing.copy_name) {
                        table_file = &file;
                        break;
                    }
                }
                break;
            }
            if (bundle == nullptr || table_file == nullptr || !safe_copy_name(missing.copy_name))
                continue;
            // A movie or a track at another size is recorded as not in the
            // source, with the size the source holds it at, and the bundle
            // plays without it. An archive at another size stopped the copy
            // above.
            if (missing.different_size) {
                const std::string line =
                    missing.copy_name + " of " + std::string(bundle->english_name) +
                    " is left out: the source holds it at " + std::to_string(missing.source_size) +
                    " bytes, not " + std::to_string(table_file->size);
                say_line(request.news, line.c_str());
            }
            const fs::path record_path =
                bundle_folder(request.user_folder, *bundle) / std::string(content_record_name);
            ContentRecord record = read_content_record(record_path, &problem);
            if (record.io_error) {
                result.status = CopyStatus::failed;
                result.error =
                    player_error(problem.empty() ? "the record could not be read" : problem);
                return result;
            }
            if (!record.valid || record.bundle_id != missing.bundle_id)
                record.files.clear();
            record.bundle_id = std::string(missing.bundle_id);
            record.source = source.origin();
            record.present = true;
            record.valid = true;
            const ContentRecordFile* donor = nullptr;
            for (const ContentRecordFile& item : record.files) {
                if (item.not_in_source || item.copy_name == missing.copy_name)
                    continue;
                if (item.size != table_file->size)
                    continue;
                if (!bundles::known_hash(*table_file, item.digest))
                    continue;
                donor = &item;
                break;
            }
            const fs::path dest =
                stored_path(bundle_folder(request.user_folder, *bundle), missing.copy_name);
            if (donor != nullptr) {
                const fs::path from =
                    stored_path(bundle_folder(request.user_folder, *bundle), donor->copy_name);
                std::error_code failure;
                fs::create_directories(dest.parent_path(), failure);
                if (failure || !duplicate_file(from, dest, &problem) ||
                    !set_modified(dest, donor->modified, &problem)) {
                    result.status = CopyStatus::failed;
                    result.error =
                        player_error(problem.empty() ? "a file could not be copied" : problem);
                    return result;
                }
                ContentRecordFile stored;
                stored.copy_name = std::string(missing.copy_name);
                stored.size = donor->size;
                stored.modified = donor->modified;
                stored.digest = donor->digest;
                bool replaced = false;
                for (ContentRecordFile& item : record.files) {
                    if (item.copy_name == stored.copy_name) {
                        item = stored;
                        replaced = true;
                        break;
                    }
                }
                if (!replaced)
                    record.files.push_back(std::move(stored));
                result.files_copied =
                    narrow_count(static_cast<std::size_t>(result.files_copied) + 1);
            } else {
                ContentRecordFile stored;
                stored.copy_name = std::string(missing.copy_name);
                stored.not_in_source = true;
                stored.source_size = missing.different_size ? missing.source_size : 0;
                bool replaced = false;
                for (ContentRecordFile& item : record.files) {
                    if (item.copy_name == stored.copy_name) {
                        item.not_in_source = true;
                        item.source_size = stored.source_size;
                        replaced = true;
                        break;
                    }
                }
                if (!replaced)
                    record.files.push_back(std::move(stored));
            }
            if (!write_content_record(record_path, record, &problem)) {
                result.status = CopyStatus::failed;
                result.error =
                    player_error(problem.empty() ? "the record could not be written" : problem);
                return result;
            }
        }
        if (told)
            say_line(request.news, "Copied the game into Open Annihilation.");
        result.status = CopyStatus::finished;
        return result;
    } catch (const std::exception& failure) {
        result.status = CopyStatus::failed;
        result.error = player_error(failure.what());
        return result;
    }
}

void ensure_managed_copy(const ManagedCopyRequest& request) {
    const std::span<const bundles::Bundle> table =
        request.bundles.empty() ? bundles::bundles() : request.bundles;
    const bool present =
        request.user_folder_known && managed_base_present(request.user_folder, table);
    const StartCopy decision = start_copy(request.game_dir_named, request.unattended, present);
    if (decision == StartCopy::leave_alone || decision == StartCopy::use_copy)
        return;
    if (decision == StartCopy::refuse)
        throw std::runtime_error(std::string(unattended_copy_refusal()));
    if (!request.user_folder_known)
        return;
    if (request.installation.empty())
        return;
    std::error_code failure;
    if (fs::is_directory(request.installation, failure)) {
        FolderContentSource source(request.installation, table);
        CopyRequest copy;
        copy.user_folder = request.user_folder;
        copy.bundles = table;
        copy.news = request.news;
        const CopyResult result = copy_content(source, copy);
        if (result.status == CopyStatus::finished)
            return;
        if (result.error.empty())
            throw std::runtime_error("The game could not be copied into Open Annihilation.");
        throw std::runtime_error(result.error);
    }
    if (failure || !fs::is_regular_file(request.installation, failure) || failure)
        return;
    const auto raw_size = fs::file_size(request.installation, failure);
    if (failure)
        return;
    const bundles::Installer* installer = bundles::find_installer(narrow_size(raw_size));
    if (installer == nullptr || installer->kind != bundles::InstallerKind::windows)
        return;
    WindowsInstallerSource source(request.installation, table);
    std::string problem;
    if (!source.open(&problem)) {
        throw std::runtime_error(
            problem.empty() ? std::string("This installer could not be read.") : problem
        );
    }
    CopyRequest copy;
    copy.user_folder = request.user_folder;
    copy.bundles = table;
    copy.news = request.news;
    const CopyResult result = copy_content(source, copy);
    if (result.status == CopyStatus::finished)
        return;
    if (result.error.empty())
        throw std::runtime_error("The game could not be copied into Open Annihilation.");
    throw std::runtime_error(result.error);
}

std::span<const CopyCheckFile> copy_check_files() noexcept {
    static const CopyCheckFile files[]{
        {"totala", "rev31.gp3", k_rev_bytes},
        {"totala", "Data/2.zrb", k_zrb_bytes},
        {"campaign", "totala4.hpi", k_campaign_bytes},
    };
    return std::span<const CopyCheckFile>{files};
}

std::span<const bundles::Bundle> copy_check_bundles() noexcept {
    static constexpr oa::base::sha256::Digest rev_digest = *oa::base::sha256::parse_hex(
        "a53f655ac1f06cfd81116b22239a57fbc504829a882006ac36ae814a051a5576"
    );
    static constexpr oa::base::sha256::Digest zrb_digest = *oa::base::sha256::parse_hex(
        "4506ef02bf38e99b3ada838a695a752b18514ad9f7a05912e8b7fc83c57e491d"
    );
    static constexpr oa::base::sha256::Digest campaign_digest = *oa::base::sha256::parse_hex(
        "01ab7c7bc9b7693b44b22d3b4ef5199192cccdad368c200dc9506a07bfaf2a30"
    );
    static constexpr oa::base::sha256::Digest rev_hashes[]{rev_digest};
    static constexpr oa::base::sha256::Digest zrb_hashes[]{zrb_digest};
    static constexpr oa::base::sha256::Digest campaign_hashes[]{campaign_digest};
    static constexpr std::string_view rev_sources[]{"rev31.gp3"};
    static constexpr std::string_view zrb_sources[]{"Data/2.zrb"};
    static constexpr std::string_view campaign_sources[]{"totala4.hpi"};
    static constexpr std::string_view campaign_extends[]{"totala"};
    static const bundles::ContentFile totala_files[]{
        {"rev31.gp3", rev_sources, static_cast<uint64_t>(k_rev_bytes.size()), rev_hashes},
        {"Data/2.zrb", zrb_sources, static_cast<uint64_t>(k_zrb_bytes.size()), zrb_hashes},
    };
    static const bundles::ContentFile campaign_files[]{
        {"totala4.hpi",
         campaign_sources,
         static_cast<uint64_t>(k_campaign_bytes.size()),
         campaign_hashes},
    };
    static const bundles::Bundle table[]{
        {"totala", "Total Annihilation", bundles::Kind::base_game, {}, {}, totala_files},
        {"campaign", "Campaign", bundles::Kind::expansion, campaign_extends, {}, campaign_files},
    };
    return std::span<const bundles::Bundle>{table};
}

int run_content_copy_check(const fs::path& documents) {
    std::string reason;
    try {
        std::error_code failure;
        const fs::path user = documents / std::string(oa::platform::preferences::user_folder_name);
        const fs::path install = documents / "G02-fixture-install";
        fs::remove_all(user, failure);
        fs::remove_all(install, failure);
        fs::create_directories(documents, failure);
        if (failure) {
            reason = "the scratch folder could not be created";
            std::cout << "content copy check: FAILED: " << reason << '\n';
            return 1;
        }
        for (const CopyCheckFile& file : copy_check_files()) {
            if (!write_fixture_file(install, file.copy_name, file.bytes, reason)) {
                std::cout << "content copy check: FAILED: " << reason << '\n';
                return 1;
            }
        }
        if (!write_fixture_file(install, "notes.txt", "leave-this\n", reason)) {
            std::cout << "content copy check: FAILED: " << reason << '\n';
            return 1;
        }
        std::vector<SourceStamp> stamps;
        if (!stamp_tree(install, stamps, reason)) {
            std::cout << "content copy check: FAILED: " << reason << '\n';
            return 1;
        }
        FolderContentSource source(install, copy_check_bundles());
        std::atomic<bool> stop{false};
        StopAfterFile stop_state;
        stop_state.flag = &stop;
        CopyRequest request;
        request.user_folder = user;
        request.bundles = copy_check_bundles();
        request.chunk_bytes = 4;
        request.stop = &stop;
        request.on_progress = stop_after_one_file;
        request.progress_context = &stop_state;
        const CopyResult first = copy_content(source, request);
        if (first.status != CopyStatus::stopped || first.files_copied < 1) {
            reason = first.error.empty() ? "the copy did not stop after a file" : first.error;
            std::cout << "content copy check: FAILED: " << reason << '\n';
            return 1;
        }
        if (!stamps_unchanged(stamps, reason)) {
            std::cout << "content copy check: FAILED: " << reason << '\n';
            return 1;
        }
        request.stop = nullptr;
        request.on_progress = nullptr;
        const CopyResult second = copy_content(source, request);
        if (second.status != CopyStatus::finished) {
            reason = second.error.empty() ? "the copy did not finish" : second.error;
            std::cout << "content copy check: FAILED: " << reason << '\n';
            return 1;
        }
        if (!stamps_unchanged(stamps, reason)) {
            std::cout << "content copy check: FAILED: " << reason << '\n';
            return 1;
        }
        for (const CopyCheckFile& file : copy_check_files()) {
            const bundles::Bundle* bundle = nullptr;
            for (const bundles::Bundle& candidate : copy_check_bundles()) {
                if (candidate.id == file.bundle_id)
                    bundle = &candidate;
            }
            if (bundle == nullptr) {
                std::cout << "content copy check: FAILED: a bundle was missing\n";
                return 1;
            }
            const fs::path dest = stored_path(bundle_folder(user, *bundle), file.copy_name);
            std::string bytes;
            if (!load_bytes(dest, bytes, reason) || bytes != file.bytes) {
                std::cout << "content copy check: FAILED: a copied file differs\n";
                return 1;
            }
            const fs::path record_path =
                bundle_folder(user, *bundle) / std::string(content_record_name);
            const ContentRecord record = read_content_record(record_path, &reason);
            if (!record.valid) {
                std::cout << "content copy check: FAILED: the record could not be read\n";
                return 1;
            }
            const auto digest = oa::base::sha256::digest_of(
                std::span<const uint8_t>(
                    reinterpret_cast<const uint8_t*>(file.bytes.data()), file.bytes.size()
                )
            );
            bool found = false;
            for (const ContentRecordFile& item : record.files) {
                if (item.copy_name == file.copy_name && item.digest == digest &&
                    item.size == file.bytes.size())
                    found = true;
            }
            if (!found) {
                std::cout << "content copy check: FAILED: the record does not name the file\n";
                return 1;
            }
        }
        if (fs::exists(
                user / std::string(managed_games_folder) / "totala" / "notes.txt", failure
            )) {
            std::cout << "content copy check: FAILED: a file that is not part of the game was "
                         "copied\n";
            return 1;
        }
        if (partial_left(user)) {
            std::cout << "content copy check: FAILED: a partial file was left\n";
            return 1;
        }
    } catch (const std::exception& failure) {
        std::cout << "content copy check: FAILED: " << failure.what() << '\n';
        return 1;
    }
    std::cout << "content copy check: passed\n";
    return 0;
}

namespace {

/// Replaces every `token` in `text` with `value`.
std::string replace_token(std::string_view text, std::string_view token, std::string_view value) {
    std::string out;
    out.reserve(text.size() + value.size());
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t at = text.find(token, start);
        if (at == std::string_view::npos) {
            out.append(text.substr(start));
            break;
        }
        out.append(text.substr(start, at - start));
        out.append(value);
        start = at + token.size();
    }
    return out;
}

/// "1 of {total} files differs" or "{count} of {total} files differ".
std::string differ_count_line(uint32_t files_differ, uint32_t file_count) {
    const std::string total = std::to_string(file_count);
    if (files_differ == 1) {
        const std::string_view pattern =
            oa::data::languages::interface_text("1 of {total} files differs");
        return replace_token(pattern, "{total}", total);
    }
    const std::string_view pattern =
        oa::data::languages::interface_text("{count} of {total} files differ");
    return replace_token(
        replace_token(pattern, "{count}", std::to_string(files_differ)), "{total}", total
    );
}

void count_up(uint32_t& count) noexcept {
    if (count < std::numeric_limits<uint32_t>::max())
        ++count;
}

} // namespace

bool content_notice_due(const ContentCheck& check) noexcept {
    for (const ContentBundleCheck& bundle : check.bundles) {
        if (bundle.files_differ > 0)
            return true;
    }
    return false;
}

bool show_content_notice(
    bool game_dir_named, bool unwatched, bool closed_this_run, const ContentCheck& check
) noexcept {
    if (game_dir_named || unwatched || closed_this_run)
        return false;
    return content_notice_due(check);
}

ContentNoticeCopy content_difference_notice(const ContentCheck& check) {
    ContentNoticeCopy copy;
    copy.title = std::string(oa::data::languages::interface_text("Game content"));
    copy.lines.push_back(
        std::string(
            oa::data::languages::interface_text(
                "The following content for your game does not match our expected sizes:"
            )
        )
    );
    for (const ContentBundleCheck& bundle : check.bundles) {
        if (bundle.files_differ == 0)
            continue;
        copy.lines.push_back(
            std::string(oa::data::languages::interface_text(bundle.english_name)) + " / " +
            differ_count_line(bundle.files_differ, bundle.file_count)
        );
    }
    copy.lines.push_back(
        std::string(
            oa::data::languages::interface_text(
                "The game still plays with these files, but network games with other players may "
                "not "
                "start, and recordings may not replay the same. Copy the content again from "
                "another "
                "source to fix it."
            )
        )
    );
    copy.details = std::string(oa::data::languages::interface_text("Details…"));
    copy.ok = std::string(oa::data::languages::interface_text("OK"));
    return copy;
}

ContentNoticeCopy content_file_notice(const ContentCheck& check) {
    ContentNoticeCopy copy;
    copy.title = std::string(oa::data::languages::interface_text("Game content"));
    copy.lines.push_back(std::string(oa::data::languages::interface_text("Files that differ:")));
    for (const ContentBundleCheck& bundle : check.bundles) {
        for (const ContentFileDifference& file : bundle.differing)
            copy.lines.push_back(
                std::string(oa::data::languages::interface_text(bundle.english_name)) + ": " +
                file.copy_name
            );
    }
    copy.ok = std::string(oa::data::languages::interface_text("OK"));
    return copy;
}

std::string content_tracks_line(uint32_t held, uint32_t total) {
    const std::string_view pattern =
        oa::data::languages::interface_text("{held} of {total} tracks");
    return replace_token(
        replace_token(pattern, "{held}", std::to_string(held)), "{total}", std::to_string(total)
    );
}

std::string content_bundle_line(const ContentBundleCheck& bundle) {
    if (bundle.files_differ > 0)
        return differ_count_line(bundle.files_differ, bundle.file_count);
    if (bundle.id == "music" && bundle.files_not_in_source > 0 &&
        bundle.files_not_in_source <= bundle.file_count) {
        return content_tracks_line(
            bundle.file_count - bundle.files_not_in_source, bundle.file_count
        );
    }
    if (bundle.left_out.size() == 1) {
        const ContentFileLeftOut& file = bundle.left_out.front();
        return replace_token(
            replace_token(
                replace_token(
                    oa::data::languages::interface_text(
                        "{file} left out: {size} bytes, not {expected}"
                    ),
                    "{file}",
                    file.copy_name
                ),
                "{size}",
                std::to_string(file.source_size)
            ),
            "{expected}",
            std::to_string(file.size)
        );
    }
    if (!bundle.left_out.empty()) {
        std::string names;
        for (const ContentFileLeftOut& file : bundle.left_out) {
            if (!names.empty())
                names += ", ";
            names += file.copy_name;
        }
        return replace_token(
            replace_token(
                oa::data::languages::interface_text(
                    "{count} files left out for their size: {files}"
                ),
                "{count}",
                std::to_string(bundle.left_out.size())
            ),
            "{files}",
            names
        );
    }
    return content_match_line(0);
}

std::string content_match_line(uint32_t files_differ) {
    if (files_differ == 0)
        return std::string(oa::data::languages::interface_text("All files match"));
    if (files_differ == 1)
        return std::string(oa::data::languages::interface_text("1 file differs"));
    return replace_token(
        oa::data::languages::interface_text("{count} files differ"),
        "{count}",
        std::to_string(files_differ)
    );
}

namespace {

std::vector<ContentBundleCheck>& kept_copy_checks() {
    static std::vector<ContentBundleCheck> kept;
    return kept;
}

/// The result this run's copy already produced for `id`, taken so the next
/// check reads that bundle. Empty when this run did not keep `id`.
std::optional<ContentBundleCheck> take_kept_copy_check(std::string_view id) {
    std::vector<ContentBundleCheck>& kept = kept_copy_checks();
    for (std::size_t index = 0; index < kept.size(); ++index) {
        if (kept[index].id != id)
            continue;
        ContentBundleCheck found = std::move(kept[index]);
        kept.erase(kept.begin() + static_cast<std::ptrdiff_t>(index));
        return found;
    }
    return std::nullopt;
}

} // namespace

void keep_this_run_copy_check(ContentCheck check) {
    kept_copy_checks() = std::move(check.bundles);
}

ContentCheck check_managed_content(
    const fs::path& user_folder,
    std::span<const bundles::Bundle> table,
    ContentCheckProgress on_progress,
    void* progress_context,
    const std::atomic<bool>* stop
) {
    const std::span<const bundles::Bundle> known = table.empty() ? bundles::bundles() : table;
    ContentCheck check;
    uint32_t files_total = 0;
    for (const bundles::Bundle& bundle : known) {
        const fs::path folder = bundle_folder(user_folder, bundle);
        const fs::path record_path = folder / std::string(content_record_name);
        const ContentRecord loaded = read_content_record(record_path, nullptr);
        if (!loaded.present || !loaded.valid || loaded.files.empty() ||
            loaded.bundle_id != bundle.id)
            continue;
        const uint32_t count = narrow_count(bundle.files.size());
        if (files_total > std::numeric_limits<uint32_t>::max() - count)
            files_total = std::numeric_limits<uint32_t>::max();
        else
            files_total += count;
    }
    uint32_t files_done = 0;
    if (on_progress != nullptr && !on_progress(0, files_total, progress_context))
        return check;
    for (const bundles::Bundle& bundle : known) {
        const fs::path folder = bundle_folder(user_folder, bundle);
        const fs::path record_path = folder / std::string(content_record_name);
        const ContentRecord loaded = read_content_record(record_path, nullptr);
        if (!loaded.present || !loaded.valid || loaded.files.empty() ||
            loaded.bundle_id != bundle.id)
            continue;
        ContentRecord record = loaded;
        ContentBundleCheck reported;
        reported.id = std::string(bundle.id);
        reported.english_name = std::string(bundle.english_name);
        reported.kind = bundle.kind;
        reported.file_count = narrow_count(bundle.files.size());
        bool wrote = false;
        // A file the check was stopped during is not a difference: the answer
        // so far is not a finished check, and the next start reads that file.
        const auto considered = [&]() -> bool {
            if (files_done < files_total)
                ++files_done;
            if (stop != nullptr && stop->load())
                return false;
            if (on_progress != nullptr && !on_progress(files_done, files_total, progress_context))
                return false;
            return true;
        };
        // This run's copy already checked the bundle. Use that answer and
        // leave its files unread. The result was taken, so a later start
        // checks the bundle as usual.
        if (std::optional<ContentBundleCheck> kept = take_kept_copy_check(bundle.id)) {
            const uint32_t files = narrow_count(bundle.files.size());
            for (uint32_t index = 0; index < files; ++index) {
                if (!considered())
                    return check;
            }
            check.bundles.push_back(std::move(*kept));
            continue;
        }
        for (const bundles::ContentFile& file : bundle.files) {
            if (stop != nullptr && stop->load())
                return check;
            const std::size_t index = recorded_index(record, file.copy_name);
            const bool recorded = index < record.files.size();
            const fs::path dest = stored_path(folder, file.copy_name);
            std::error_code failure;
            const bool regular = fs::is_regular_file(dest, failure);
            const auto disk_bytes = regular ? fs::file_size(dest, failure) : uintmax_t{0};
            const bool sized = regular && !failure;
            const auto stamp = sized ? fs::last_write_time(dest, failure) : fs::file_time_type{};
            const bool timed = sized && !failure;
            const bool never_held = recorded && record.files[index].not_in_source;
            const uint64_t held_size = never_held ? record.files[index].source_size : 0;
            // A file the copy left out because the source held it at another
            // size is not a difference while it is still absent: it is named,
            // with both sizes.
            if (held_size > 0 && !regular) {
                count_up(reported.files_not_in_source);
                ContentFileLeftOut left_out;
                left_out.copy_name = std::string(file.copy_name);
                left_out.source_size = held_size;
                left_out.size = file.size;
                reported.left_out.push_back(std::move(left_out));
                if (!considered())
                    return check;
                continue;
            }
            // A music file the source never held is not a difference while it
            // is still absent. A file that is present is checked as usual, so
            // a changed or damaged track is still named.
            if (never_held && bundle.id == "music" && !regular) {
                count_up(reported.files_not_in_source);
                if (!considered())
                    return check;
                continue;
            }
            const bool unchanged = recorded && timed && !never_held &&
                                   narrow_size(disk_bytes) == record.files[index].size &&
                                   modified_seconds(stamp) == record.files[index].modified;
            oa::base::sha256::Digest digest{};
            bool have_digest = false;
            if (unchanged) {
                digest = record.files[index].digest;
                have_digest = true;
            } else if (regular) {
                uint64_t hashed_size = 0;
                int64_t hashed_time = 0;
                if (hash_whole_file(dest, digest, hashed_size, hashed_time, stop)) {
                    have_digest = true;
                    count_up(check.files_hashed);
                    ContentRecordFile updated;
                    updated.copy_name = std::string(file.copy_name);
                    updated.size = hashed_size;
                    updated.modified = hashed_time;
                    updated.digest = digest;
                    updated.not_in_source = never_held;
                    updated.source_size = held_size;
                    if (recorded)
                        record.files[index] = std::move(updated);
                    else
                        record.files.push_back(std::move(updated));
                    wrote = true;
                } else if (stop != nullptr && stop->load()) {
                    return check;
                }
            }
            const bool differs = !have_digest || !bundles::known_hash(file, digest);
            if (never_held && bundle.id == "music" && regular && !differs && recorded) {
                record.files[index].not_in_source = false;
                wrote = true;
            }
            if (differs) {
                count_up(reported.files_differ);
                ContentFileDifference difference;
                difference.copy_name = std::string(file.copy_name);
                reported.differing.push_back(std::move(difference));
            }
            if (!considered())
                return check;
        }
        // A record that cannot be stored is hashed again next start. This
        // start's answer stands either way.
        if (wrote) {
            const bool stored = write_content_record(record_path, record, nullptr);
            static_cast<void>(stored);
        }
        check.bundles.push_back(std::move(reported));
    }
    return check;
}

int run_installer_check(const fs::path& file) {
    std::error_code error;
    const uintmax_t size = fs::file_size(file, error);
    const bundles::Installer* known = error ? nullptr : bundles::find_installer(size);
    if (known != nullptr && known->kind == bundles::InstallerKind::older_mac)
        return run_older_mac_installer_check(file);
    if (known != nullptr && known->kind == bundles::InstallerKind::macos)
        return run_macos_installer_check(file);
    return run_windows_installer_check(file);
}

} // namespace oa::app
