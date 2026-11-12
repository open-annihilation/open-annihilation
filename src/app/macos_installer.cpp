// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The macOS installer as a source of game files. The payload is a cpio
// stream read once: each game file is written as it passes, and the rest of
// the payload is left where it is.

#include "oa/app/content_copy.hpp"

#include "oa/app/game_directory.hpp"
#include "oa/base/sha256.hpp"
#include "oa/formats/cpio.hpp"
#include "oa/formats/xar.hpp"
#include "oa/platform/file_clone.hpp"
#include "oa/platform/file_time.hpp"
#include "oa/platform/files.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
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

namespace fs = std::filesystem;
namespace bundles = oa::data::content_bundles;

constexpr std::size_t k_read_bytes = 1024 * 1024;

struct OwnedFile {
    std::FILE* file = nullptr;

    ~OwnedFile() { close(); }

    OwnedFile() = default;

    explicit OwnedFile(std::FILE* opened) : file(opened) {}

    OwnedFile(const OwnedFile&) = delete;
    OwnedFile& operator=(const OwnedFile&) = delete;

    OwnedFile(OwnedFile&& other) noexcept : file(other.file) { other.file = nullptr; }

    OwnedFile& operator=(OwnedFile&& other) noexcept {
        if (this != &other) {
            close();
            file = other.file;
            other.file = nullptr;
        }
        return *this;
    }

    void close() noexcept {
        if (file != nullptr)
            std::fclose(file);
        file = nullptr;
    }
};

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

void say_line(std::ostream* news, const char* line) {
    if (news != nullptr)
        *news << "open-annihilation: " << line << '\n';
}

std::string sentence(std::string_view detail) {
    if (detail.empty())
        return "The installer could not be read.";
    std::string text(detail);
    if (text.front() >= 'a' && text.front() <= 'z')
        text.front() = static_cast<char>(text.front() - 'a' + 'A');
    if (text.back() != '.')
        text.push_back('.');
    return text;
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

[[nodiscard]] uint64_t narrow_size(uintmax_t value) noexcept {
    constexpr auto limit = static_cast<uintmax_t>(std::numeric_limits<uint64_t>::max());
    if (value > limit)
        return std::numeric_limits<uint64_t>::max();
    return static_cast<uint64_t>(value);
}

[[nodiscard]] uint32_t narrow_count(std::size_t count) noexcept {
    constexpr auto limit = static_cast<std::size_t>(std::numeric_limits<uint32_t>::max());
    if (count > limit)
        return std::numeric_limits<uint32_t>::max();
    return static_cast<uint32_t>(count);
}

void add_bytes(uint64_t& total, uint64_t more) noexcept {
    if (total > std::numeric_limits<uint64_t>::max() - more)
        total = std::numeric_limits<uint64_t>::max();
    else
        total += more;
}

[[nodiscard]] bool seek_file(std::FILE* file, uint64_t offset) noexcept {
#if defined(_WIN32)
    return _fseeki64(file, static_cast<__int64>(offset), SEEK_SET) == 0;
#else
    return ::fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

[[nodiscard]] bool truncate_file(std::FILE* file, uint64_t length) noexcept {
#if defined(_WIN32)
    // _chsize, not _chsize_s: Windows 95's C runtime has only the first. Its
    // length is a long, and no game file comes near 2 GiB.
    if (std::fflush(file) != 0 || length > static_cast<uint64_t>(std::numeric_limits<long>::max()))
        return false;
    return _chsize(_fileno(file), static_cast<long>(length)) == 0;
#else
    if (length > static_cast<uint64_t>(std::numeric_limits<off_t>::max()))
        return false;
    return ::ftruncate(::fileno(file), static_cast<off_t>(length)) == 0;
#endif
}

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

[[nodiscard]] bool read_full(std::FILE* file, uint8_t* dest, std::size_t bytes) noexcept {
    std::size_t got = 0;
    while (got < bytes) {
        const std::size_t read = std::fread(dest + got, 1, bytes - got, file);
        if (read == 0)
            return false;
        got += read;
    }
    return true;
}

[[nodiscard]] bool hash_file(std::FILE* file, oa::base::sha256::Digest& digest) {
    if (!seek_file(file, 0))
        return false;
    oa::base::sha256::Hasher hasher;
    std::vector<uint8_t> buffer(k_read_bytes);
    while (true) {
        const std::size_t read = std::fread(buffer.data(), 1, buffer.size(), file);
        if (read > 0)
            oa::base::sha256::update(hasher, std::span<const uint8_t>(buffer.data(), read));
        if (read < buffer.size())
            break;
    }
    if (std::ferror(file) != 0)
        return false;
    digest = oa::base::sha256::finish(hasher);
    return true;
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
    return oa::platform::file_seconds_since_1970(stamp) == recorded.modified;
}

[[nodiscard]] const ContentRecordFile*
find_recorded(const ContentRecord& record, std::string_view copy_name) noexcept {
    for (const ContentRecordFile& item : record.files) {
        if (item.copy_name == copy_name)
            return &item;
    }
    return nullptr;
}

[[nodiscard]] fs::path existing_ancestor(fs::path directory) {
    std::error_code failure;
    while (!directory.empty()) {
        if (fs::is_directory(directory, failure))
            return directory;
        const fs::path parent = directory.parent_path();
        if (parent == directory)
            break;
        directory = parent;
    }
    return {};
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

[[nodiscard]] uint64_t chunk_size(uint64_t requested) noexcept {
    if (requested == 0 || requested > content_copy_chunk_bytes)
        return content_copy_chunk_bytes;
    return requested;
}

enum class Job : uint8_t { list, copy, stream, check };

enum class Halt : uint8_t { none, stop, fail };

struct Slot {
    const bundles::Bundle* bundle{};
    const bundles::ContentFile* file{};
    std::size_t bundle_index{};
    bool seen{};
    bool complete{};
    fs::path dest{};
    fs::path partial{};
    uint64_t prefix{};
    oa::base::sha256::Digest digest{};
    bool have_digest{};
    /// Set when the bytes come from another file of this bundle.
    std::string_view filled_from{};
    /// The installer has this name at a different, non-empty size.
    bool different_size{};
};

struct Target {
    std::size_t slot{std::numeric_limits<std::size_t>::max()};
    OwnedFile part{};
    bool comparing{};
    bool hashing_only{};
    bool rewound{};
    uint64_t prefix{};
    uint64_t filled{};
    oa::base::sha256::Hasher hasher{};
};

struct Active {
    bool on{};
    std::vector<Target> targets{};
};

struct PendingLink {
    uint64_t device{};
    uint64_t inode{};
    std::size_t slot{};
};

struct Pump {
    std::span<const bundles::Bundle> bundles{};
    std::string origin_text{};
    Job job{Job::list};
    const CopyRequest* request{};
    CopyResult* result{};
    std::vector<ContentSourceFile>* listed{};
    const StreamRequest* stream_request{};
    StreamResult* stream_result{};
    std::string* error{};
    std::vector<Slot> slots{};
    std::vector<PendingLink> pending{};
    std::vector<ContentRecord> records{};
    Active active{};
    Halt halt{Halt::none};
    bool told{};
    bool found_stream{};
    bool no_space{};
    uint64_t chunk{content_copy_chunk_bytes};
    uint64_t done_bytes{};
    uint32_t done_files{};
    uint64_t bytes_total{};
    /// Files this copy takes. Zero until the archive has been read to the end,
    /// while the running count still uses every table slot.
    uint32_t counted_total{};

    void fail(std::string_view text) {
        halt = Halt::fail;
        set_error(error, text);
    }

    [[nodiscard]] std::size_t match_name(std::string_view name) const {
        std::vector<std::string_view> parts;
        std::size_t start = 0;
        for (std::size_t index = 0; index <= name.size(); ++index) {
            if (index != name.size() && name[index] != '/' && name[index] != '\\')
                continue;
            if (index > start)
                parts.push_back(name.substr(start, index - start));
            start = index + 1;
        }
        std::size_t drive = std::numeric_limits<std::size_t>::max();
        for (std::size_t index = 0; index < parts.size(); ++index) {
            if (folded_equal(parts[index], "drive_c")) {
                drive = index;
                break;
            }
        }
        if (drive == std::numeric_limits<std::size_t>::max())
            return std::numeric_limits<std::size_t>::max();
        for (std::size_t from = drive + 1; from < parts.size(); ++from) {
            std::string suffix;
            for (std::size_t index = from; index < parts.size(); ++index) {
                if (!suffix.empty())
                    suffix.push_back('/');
                suffix.append(parts[index]);
            }
            for (std::size_t slot = 0; slot < slots.size(); ++slot) {
                if (names_match(suffix, *slots[slot].file))
                    return slot;
            }
        }
        return std::numeric_limits<std::size_t>::max();
    }

    void note_progress(const Slot& slot, bool finished) {
        if (request == nullptr || request->on_progress == nullptr || result == nullptr)
            return;
        CopyProgress progress;
        progress.bytes_done = done_bytes;
        if (!finished && active.on) {
            for (const Target& target : active.targets)
                add_bytes(progress.bytes_done, target.filled);
        }
        progress.bytes_total = bytes_total;
        progress.files_done = done_files;
        progress.files_total = counted_total > 0 ? counted_total : narrow_count(slots.size());
        progress.bundle_id = slot.bundle->id;
        progress.copy_name = slot.file->copy_name;
        progress.file_finished = finished;
        request->on_progress(request->progress_context, progress);
    }

    [[nodiscard]] bool stopped_now() const noexcept {
        const std::atomic<bool>* flag = nullptr;
        if (job == Job::copy && request != nullptr)
            flag = request->stop;
        if (job == Job::stream && stream_request != nullptr)
            flag = stream_request->stop;
        return flag != nullptr && flag->load();
    }

    [[nodiscard]] bool prepare_slots() {
        if (bundles.empty())
            return true;
        records.assign(bundles.size(), ContentRecord{});
        if (job == Job::copy) {
            for (std::size_t index = 0; index < bundles.size(); ++index) {
                const fs::path record_path = bundle_folder(request->user_folder, bundles[index]) /
                                             std::string(content_record_name);
                std::string problem;
                records[index] = read_content_record(record_path, &problem);
                if (records[index].io_error) {
                    fail(problem.empty() ? "the record could not be read" : problem);
                    return false;
                }
                if (!(records[index].valid && records[index].bundle_id == bundles[index].id))
                    records[index].files.clear();
                records[index].bundle_id = std::string(bundles[index].id);
                records[index].source = origin_text;
                records[index].valid = true;
                records[index].present = true;
            }
        }
        for (std::size_t bundle_index = 0; bundle_index < bundles.size(); ++bundle_index) {
            const bundles::Bundle& bundle = bundles[bundle_index];
            for (const bundles::ContentFile& file : bundle.files) {
                Slot slot;
                slot.bundle = &bundle;
                slot.file = &file;
                slot.bundle_index = bundle_index;
                add_bytes(bytes_total, file.size);
                if (job == Job::copy) {
                    if (!safe_copy_name(file.copy_name)) {
                        fail("a file name cannot be stored");
                        return false;
                    }
                    slot.dest =
                        stored_path(bundle_folder(request->user_folder, bundle), file.copy_name);
                    slot.partial = partial_path(slot.dest);
                    const ContentRecordFile* recorded =
                        find_recorded(records[bundle_index], file.copy_name);
                    slot.complete =
                        recorded != nullptr && file_matches_record(slot.dest, *recorded, file.size);
                    if (!slot.complete) {
                        slot.prefix = regular_size(slot.partial);
                        if (slot.prefix > file.size) {
                            remove_file(slot.partial);
                            slot.prefix = 0;
                        }
                    }
                }
                slots.push_back(std::move(slot));
            }
        }
        if (job != Job::copy)
            return true;
        uint64_t need_bytes = 0;
        for (const Slot& slot : slots) {
            if (!slot.complete)
                add_bytes(need_bytes, slot.file->size);
        }
        if (need_bytes == 0)
            return true;
        const uint64_t need = bytes_to_make_room_for(need_bytes, false);
        const FreeSpace free = query_free(*request, request->user_folder);
        if (!room_is_enough(free.bytes, free.known, need)) {
            no_space = true;
            fail("short space");
            return false;
        }
        return true;
    }

    void skip_slot(Slot& slot) {
        if (job != Job::copy || result == nullptr)
            return;
        result->files_skipped = narrow_count(static_cast<std::size_t>(result->files_skipped) + 1);
        add_bytes(done_bytes, slot.file->size);
        done_files = narrow_count(static_cast<std::size_t>(done_files) + 1);
        note_progress(slot, true);
    }

    [[nodiscard]] bool open_targets(const std::vector<std::size_t>& chosen) {
        active = {};
        for (const std::size_t index : chosen) {
            Slot& slot = slots[index];
            Target target;
            target.slot = index;
            if (job == Job::check) {
                target.hashing_only = true;
                active.targets.push_back(std::move(target));
                continue;
            }
            if (job == Job::stream) {
                found_stream = true;
                target.prefix = regular_size(stream_request->destination);
                if (target.prefix > slot.file->size) {
                    remove_file(stream_request->destination);
                    target.prefix = 0;
                }
                const char* mode = target.prefix == 0 ? "wb" : "r+b";
                target.part = OwnedFile(oa::platform::open_file(stream_request->destination, mode));
                if (target.part.file == nullptr) {
                    fail("the copy could not be written");
                    return false;
                }
                target.comparing = target.prefix > 0;
                active.targets.push_back(std::move(target));
                continue;
            }
            if (stopped_now()) {
                halt = Halt::stop;
                return false;
            }
            std::error_code failure;
            fs::create_directories(slot.dest.parent_path(), failure);
            if (failure) {
                fail("the folder could not be created");
                return false;
            }
            remove_file(slot.dest);
            target.prefix = slot.prefix;
            const char* mode = target.prefix == 0 ? "wb" : "r+b";
            target.part = OwnedFile(oa::platform::open_file(slot.partial, mode));
            if (target.part.file == nullptr) {
                fail("the copy could not be written");
                return false;
            }
            target.comparing = target.prefix > 0;
            active.targets.push_back(std::move(target));
            if (!told) {
                say_line(request->news, "Copying the game into Open Annihilation...");
                told = true;
            }
        }
        active.on = !active.targets.empty();
        return true;
    }

    [[nodiscard]] bool write_bytes(std::FILE* file, std::span<const uint8_t> bytes) {
        if (bytes.empty())
            return true;
        errno = 0;
        if (std::fwrite(bytes.data(), 1, bytes.size(), file) != bytes.size()) {
            if (disk_is_full(errno)) {
                no_space = true;
                fail("the disk filled while the game was copied");
                return false;
            }
            fail("the copy could not be written");
            return false;
        }
        return true;
    }

    [[nodiscard]] bool accept_bytes(Target& target, std::span<const uint8_t> bytes) {
        while (!bytes.empty()) {
            std::size_t step = std::min<std::size_t>(bytes.size(), static_cast<std::size_t>(chunk));
            if (target.comparing) {
                const uint64_t remain = target.prefix - target.filled;
                step = std::min<std::size_t>(step, static_cast<std::size_t>(remain));
                std::vector<uint8_t> have(step);
                const std::size_t got = std::fread(have.data(), 1, step, target.part.file);
                if (std::ferror(target.part.file) != 0) {
                    fail("the copy could not be read");
                    return false;
                }
                std::size_t same = 0;
                while (same < got && have[same] == bytes[same])
                    ++same;
                oa::base::sha256::update(target.hasher, bytes.first(same));
                target.filled += same;
                if (same < step) {
                    target.rewound = true;
                    target.comparing = false;
                    if (!truncate_file(target.part.file, target.filled) ||
                        !seek_file(target.part.file, target.filled)) {
                        fail("the copy could not be written");
                        return false;
                    }
                    const auto rest = bytes.subspan(same, step - same);
                    oa::base::sha256::update(target.hasher, rest);
                    if (!write_bytes(target.part.file, rest))
                        return false;
                    target.filled += rest.size();
                } else if (target.filled >= target.prefix) {
                    target.comparing = false;
                }
            } else {
                const auto piece = bytes.first(step);
                oa::base::sha256::update(target.hasher, piece);
                if (!target.hashing_only && !write_bytes(target.part.file, piece))
                    return false;
                target.filled += step;
            }
            bytes = bytes.subspan(step);
        }
        return true;
    }

    [[nodiscard]] bool take_bytes(std::span<const uint8_t> bytes) {
        while (!bytes.empty()) {
            if (stopped_now()) {
                for (Target& target : active.targets) {
                    if (!target.hashing_only && target.part.file != nullptr &&
                        !sync_file(target.part.file)) {
                        fail("the copy could not be stored");
                        return false;
                    }
                }
                halt = Halt::stop;
                return false;
            }
            const std::size_t step =
                std::min<std::size_t>(bytes.size(), static_cast<std::size_t>(chunk));
            const auto piece = bytes.first(step);
            for (Target& target : active.targets) {
                if (!accept_bytes(target, piece))
                    return false;
            }
            bytes = bytes.subspan(step);
            if (job == Job::copy) {
                for (const Target& target : active.targets)
                    note_progress(slots[target.slot], false);
            }
            if (job == Job::stream && stream_request->on_chunk != nullptr &&
                !active.targets.empty())
                stream_request->on_chunk(
                    stream_request->chunk_context, active.targets.front().filled
                );
        }
        return true;
    }

    [[nodiscard]] bool remember(Slot& slot, int64_t modified, bool not_in_source) {
        ContentRecord& record = records[slot.bundle_index];
        ContentRecordFile stored;
        stored.copy_name = std::string(slot.file->copy_name);
        stored.size = not_in_source ? 0 : slot.file->size;
        stored.modified = not_in_source ? 0 : modified;
        stored.digest = not_in_source ? oa::base::sha256::Digest{} : slot.digest;
        stored.not_in_source = not_in_source;
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
        const fs::path record_path =
            bundle_folder(request->user_folder, *slot.bundle) / std::string(content_record_name);
        std::string problem;
        if (!write_content_record(record_path, record, &problem)) {
            fail(problem.empty() ? "the record could not be written" : problem);
            return false;
        }
        return true;
    }

    [[nodiscard]] bool stamp(const fs::path& path, int64_t modified, int64_t& stored) {
        std::error_code failure;
        fs::last_write_time(path, oa::platform::file_time_from_seconds(modified), failure);
        if (failure) {
            fail("the copied file's time could not be set");
            return false;
        }
        const auto read_back = fs::last_write_time(path, failure);
        if (failure) {
            fail("the copied file's time could not be set");
            return false;
        }
        stored = oa::platform::file_seconds_since_1970(read_back);
        return true;
    }

    [[nodiscard]] bool finish_one(Target& target) {
        Slot& slot = slots[target.slot];
        if (target.filled != slot.file->size) {
            fail("the installer could not be read");
            return false;
        }
        slot.digest = oa::base::sha256::finish(target.hasher);
        slot.have_digest = true;
        if (target.hashing_only)
            return true;
        if (job == Job::stream) {
            if (!sync_file(target.part.file)) {
                fail("the copy could not be stored");
                return false;
            }
            target.part.close();
            stream_result->digest = slot.digest;
            stream_result->bytes = slot.file->size;
            return true;
        }
        if (!sync_file(target.part.file)) {
            if (disk_is_full(errno)) {
                no_space = true;
                fail("the disk filled while the game was copied");
                return false;
            }
            fail("the copy could not be stored");
            return false;
        }
        target.part.close();
        int64_t stored_time = 0;
        if (!stamp(slot.partial, cpio_modified, stored_time))
            return false;
        std::error_code failure;
        fs::rename(slot.partial, slot.dest, failure);
        if (failure) {
            remove_file(slot.dest);
            fs::rename(slot.partial, slot.dest, failure);
            if (failure) {
                fail("a copied file could not be stored");
                return false;
            }
        }
        if (!stamp(slot.dest, cpio_modified, stored_time))
            return false;
        if (!remember(slot, stored_time, false))
            return false;
        const uint64_t fresh = target.rewound ? slot.file->size : slot.file->size - target.prefix;
        add_bytes(result->bytes_copied, fresh);
        result->files_copied = narrow_count(static_cast<std::size_t>(result->files_copied) + 1);
        add_bytes(done_bytes, slot.file->size);
        done_files = narrow_count(static_cast<std::size_t>(done_files) + 1);
        note_progress(slot, true);
        return true;
    }

    /// Once the archive has been read, the overall count is the files this
    /// copy took. A file the installer did not hold is left out, so a finished
    /// copy reads 100%.
    void settle_taken() {
        if (job != Job::copy || slots.empty())
            return;
        uint32_t taken = 0;
        uint64_t bytes = 0;
        const Slot* sample = nullptr;
        for (const Slot& slot : slots) {
            if (!(slot.complete || (slot.seen && slot.have_digest)))
                continue;
            taken = narrow_count(static_cast<std::size_t>(taken) + 1);
            add_bytes(bytes, slot.file->size);
            if (sample == nullptr)
                sample = &slot;
        }
        const uint32_t table_files = narrow_count(slots.size());
        const uint32_t not_taken = table_files >= taken ? table_files - taken : 0;
        const CopyFileCount tally = copy_file_count(taken, table_files, not_taken);
        done_files = tally.done;
        counted_total = tally.total;
        done_bytes = bytes;
        if (bytes > 0)
            bytes_total = bytes;
        if (sample != nullptr)
            note_progress(*sample, true);
    }

    [[nodiscard]] bool finish_active() {
        if (!active.on)
            return true;
        for (Target& target : active.targets) {
            if (!finish_one(target))
                return false;
        }
        active.on = false;
        active.targets.clear();
        return true;
    }

    [[nodiscard]] bool bundle_held(const Slot& slot) const {
        for (const Slot& other : slots) {
            if (other.bundle != slot.bundle)
                continue;
            if (other.complete || (other.seen && other.have_digest && other.filled_from.empty()))
                return true;
        }
        return false;
    }

    // A file the installer did not hold can be the bytes of another file in
    // the same bundle, when that file's size matches and its hash is one the
    // table accepts for the missing name. A check names that file. A copy
    // writes the bytes under the missing name. A file nothing can fill is
    // recorded as not in the source.
    [[nodiscard]] bool fill_gaps() {
        if (job != Job::copy && job != Job::check)
            return true;
        for (Slot& slot : slots) {
            if (slot.complete || slot.different_size || (slot.seen && slot.have_digest))
                continue;
            const Slot* donor = nullptr;
            oa::base::sha256::Digest donor_digest{};
            for (const Slot& other : slots) {
                if (other.bundle != slot.bundle || other.file == slot.file)
                    continue;
                if (other.file->size != slot.file->size)
                    continue;
                bool have = false;
                oa::base::sha256::Digest digest{};
                if (other.seen && other.have_digest && other.filled_from.empty()) {
                    digest = other.digest;
                    have = true;
                } else if (job == Job::copy && other.bundle_index < records.size()) {
                    const ContentRecordFile* recorded =
                        find_recorded(records[other.bundle_index], other.file->copy_name);
                    if (recorded != nullptr && !recorded->not_in_source &&
                        recorded->size == slot.file->size) {
                        digest = recorded->digest;
                        have = true;
                    }
                }
                if (!have || !bundles::known_hash(*slot.file, digest))
                    continue;
                donor = &other;
                donor_digest = digest;
                break;
            }
            if (donor == nullptr) {
                if (job != Job::copy || !bundle_held(slot))
                    continue;
                slot.digest = {};
                if (!remember(slot, 0, true))
                    return false;
                continue;
            }
            slot.digest = donor_digest;
            slot.have_digest = true;
            slot.seen = true;
            slot.filled_from =
                donor->filled_from.empty() ? donor->file->copy_name : donor->filled_from;
            if (job == Job::check)
                continue;
            std::error_code failure;
            fs::create_directories(slot.dest.parent_path(), failure);
            if (failure) {
                fail("the folder could not be created");
                return false;
            }
            remove_file(slot.dest);
            std::string share_error;
            bool stored =
                oa::platform::blocks_can_be_shared(donor->dest, slot.dest.parent_path()) &&
                oa::platform::share_file_blocks(donor->dest, slot.dest, &share_error);
            if (!stored) {
                fs::copy_file(
                    donor->dest, slot.dest, fs::copy_options::overwrite_existing, failure
                );
                if (failure) {
                    fail("a file could not be copied");
                    return false;
                }
            }
            int64_t stored_time = cpio_modified;
            const ContentRecordFile* recorded =
                find_recorded(records[donor->bundle_index], donor->file->copy_name);
            if (recorded != nullptr)
                stored_time = recorded->modified;
            if (!stamp(slot.dest, stored_time, stored_time))
                return false;
            if (!remember(slot, stored_time, false))
                return false;
            if (result != nullptr) {
                result->files_copied =
                    narrow_count(static_cast<std::size_t>(result->files_copied) + 1);
            }
        }
        return true;
    }

    int64_t cpio_modified{};
};

void choose_slot(std::size_t index, std::vector<std::size_t>& chosen) {
    for (const std::size_t have : chosen) {
        if (have == index)
            return;
    }
    chosen.push_back(index);
}

bool on_begin(void* context, const oa::formats::cpio::File& file, bool& take) {
    auto* pump = static_cast<Pump*>(context);
    take = false;
    if (pump->halt != Halt::none || file.trailer || !file.regular)
        return true;
    if (!oa::formats::cpio::name_is_safe(file.name))
        return true;
    std::vector<std::size_t> chosen;
    const std::size_t named = pump->match_name(file.name);
    if (named != std::numeric_limits<std::size_t>::max()) {
        Slot& slot = pump->slots[named];
        if (!slot.seen && file.bytes == slot.file->size) {
            slot.seen = true;
            if (slot.complete)
                pump->skip_slot(slot);
            else
                choose_slot(named, chosen);
        } else if (!slot.seen && file.bytes == 0 && slot.file->size > 0) {
            PendingLink link;
            link.device = file.device;
            link.inode = file.inode;
            link.slot = named;
            pump->pending.push_back(link);
        } else if (!slot.seen && file.bytes != slot.file->size && file.bytes != 0) {
            slot.different_size = true;
        }
    }
    if (file.bytes > 0) {
        std::vector<PendingLink> still;
        for (const PendingLink& link : pump->pending) {
            Slot& slot = pump->slots[link.slot];
            const bool same = link.device == file.device && link.inode == file.inode &&
                              slot.file->size == file.bytes;
            if (slot.seen || !same) {
                if (!slot.seen)
                    still.push_back(link);
                continue;
            }
            slot.seen = true;
            if (slot.complete)
                pump->skip_slot(slot);
            else
                choose_slot(link.slot, chosen);
        }
        pump->pending = std::move(still);
    }
    if (pump->job == Job::stream) {
        std::vector<std::size_t> wanted;
        for (const std::size_t index : chosen) {
            const Slot& slot = pump->slots[index];
            const bool matches = pump->stream_request->name == slot.file->copy_name ||
                                 names_match(pump->stream_request->name, *slot.file);
            if (matches && !pump->found_stream)
                wanted.push_back(index);
        }
        chosen = std::move(wanted);
    }
    if (chosen.empty())
        return true;
    if (pump->job == Job::list) {
        for (const std::size_t index : chosen) {
            const Slot& slot = pump->slots[index];
            ContentSourceFile listed;
            listed.name = std::string(slot.file->copy_name);
            listed.size = slot.file->size;
            listed.modified = file.modified;
            pump->listed->push_back(std::move(listed));
        }
        return true;
    }
    pump->cpio_modified = file.modified;
    if (!pump->open_targets(chosen))
        return false;
    take = pump->active.on;
    return true;
}

bool on_data(void* context, std::span<const uint8_t> bytes) {
    auto* pump = static_cast<Pump*>(context);
    if (!pump->active.on)
        return true;
    return pump->take_bytes(bytes);
}

bool on_end(void* context) {
    auto* pump = static_cast<Pump*>(context);
    if (!pump->finish_active())
        return false;
    if (!pump->fill_gaps())
        return false;
    pump->settle_taken();
    return true;
}

[[nodiscard]] bool feed_cpio(
    oa::formats::cpio::Reader& reader,
    const oa::formats::cpio::Visit& visit,
    std::span<const uint8_t> bytes,
    Pump& pump
) {
    std::string problem;
    if (reader.feed(bytes, visit, &problem))
        return true;
    if (reader.halted() && pump.halt == Halt::stop)
        return false;
    if (pump.halt == Halt::none)
        pump.fail(problem.empty() ? "the installer could not be read" : problem);
    return false;
}

[[nodiscard]] bool read_gzip(
    std::FILE* file,
    uint64_t bytes,
    oa::formats::cpio::Reader& reader,
    const oa::formats::cpio::Visit& visit,
    Pump& pump
) {
    oa::formats::xar::GzipReader gzip;
    std::vector<uint8_t> input(k_read_bytes);
    std::vector<uint8_t> output(k_read_bytes);
    uint64_t left = bytes;
    bool finished = false;
    while (!finished) {
        if (left == 0) {
            const oa::formats::xar::GzipFeed fed = gzip.feed({}, output);
            if (fed.failed || !fed.finished) {
                pump.fail("the installer could not be read");
                return false;
            }
            if (fed.produced > 0 &&
                !feed_cpio(
                    reader, visit, std::span<const uint8_t>(output.data(), fed.produced), pump
                ))
                return pump.halt == Halt::stop;
            finished = true;
            break;
        }
        const std::size_t want = static_cast<std::size_t>(std::min<uint64_t>(left, input.size()));
        if (!read_full(file, input.data(), want)) {
            pump.fail("the installer could not be read");
            return false;
        }
        left -= want;
        std::size_t used = 0;
        while (used < want) {
            const oa::formats::xar::GzipFeed fed =
                gzip.feed(std::span<const uint8_t>(input.data() + used, want - used), output);
            if (fed.failed) {
                pump.fail("the installer could not be read");
                return false;
            }
            if (fed.produced > 0 &&
                !feed_cpio(
                    reader, visit, std::span<const uint8_t>(output.data(), fed.produced), pump
                ))
                return pump.halt == Halt::stop;
            if (fed.consumed == 0 && fed.produced == 0 && !fed.finished) {
                pump.fail("the installer could not be read");
                return false;
            }
            used += fed.consumed;
            if (fed.finished) {
                if (used != want || left != 0) {
                    pump.fail("the installer could not be read");
                    return false;
                }
                finished = true;
                break;
            }
        }
    }
    return true;
}

[[nodiscard]] bool read_plain(
    std::FILE* file,
    uint64_t bytes,
    oa::formats::cpio::Reader& reader,
    const oa::formats::cpio::Visit& visit,
    Pump& pump
) {
    std::vector<uint8_t> input(k_read_bytes);
    uint64_t left = bytes;
    while (left > 0) {
        const std::size_t want = static_cast<std::size_t>(std::min<uint64_t>(left, input.size()));
        if (!read_full(file, input.data(), want)) {
            pump.fail("the installer could not be read");
            return false;
        }
        left -= want;
        if (!feed_cpio(reader, visit, std::span<const uint8_t>(input.data(), want), pump))
            return pump.halt == Halt::stop;
    }
    return true;
}

} // namespace

struct InstallerWalk {
    [[nodiscard]] static bool run(MacosInstallerSource& source, Pump& pump) {
        pump.bundles = source.bundles_;
        pump.origin_text = source.origin();
        if (!pump.prepare_slots())
            return false;
        if (source.bundles_.empty())
            return true;
        OwnedFile input(oa::platform::open_file(source.file_, "rb"));
        if (input.file == nullptr) {
            pump.fail("the installer could not be read");
            return false;
        }
        if (!seek_file(input.file, source.scripts_at_)) {
            pump.fail("the installer could not be read");
            return false;
        }
        bool gzip = source.outer_gzip_;
        if (!gzip && source.scripts_bytes_ >= 2) {
            uint8_t magic[2];
            if (!read_full(input.file, magic, 2) || !seek_file(input.file, source.scripts_at_)) {
                pump.fail("the installer could not be read");
                return false;
            }
            gzip = magic[0] == 0x1f && magic[1] == 0x8b;
        }
        oa::formats::cpio::Reader reader;
        oa::formats::cpio::Visit visit;
        visit.context = &pump;
        visit.begin = on_begin;
        visit.data = on_data;
        visit.end = on_end;
        const bool streamed =
            gzip ? read_gzip(input.file, source.scripts_bytes_, reader, visit, pump)
                 : read_plain(input.file, source.scripts_bytes_, reader, visit, pump);
        if (!streamed)
            return pump.halt == Halt::stop;
        std::string problem;
        if (reader.halted())
            return pump.halt == Halt::stop;
        if (!reader.finish(&problem)) {
            pump.fail(problem.empty() ? "the installer could not be read" : problem);
            return false;
        }
        if (pump.job == Job::stream && !pump.found_stream) {
            pump.fail("the file could not be found");
            return false;
        }
        return true;
    }
};

MacosInstallerSource::MacosInstallerSource(
    fs::path file, std::span<const bundles::Bundle> bundles, bool require_known
)
    : file_(std::move(file)), bundles_(bundles), require_known_(require_known) {
}

std::string MacosInstallerSource::origin() const {
    return path_to_utf8(file_);
}

bool MacosInstallerSource::ensure(std::string* error, std::ostream* news) {
    if (opened_) {
        if (!ready_)
            set_error(error, open_error_);
        return ready_;
    }
    opened_ = true;
    std::error_code failure;
    if (!fs::is_regular_file(file_, failure)) {
        open_error_ = "the installer could not be read";
        set_error(error, open_error_);
        return false;
    }
    const uint64_t size = narrow_size(fs::file_size(file_, failure));
    if (failure) {
        open_error_ = "the installer could not be read";
        set_error(error, open_error_);
        return false;
    }
    OwnedFile input(oa::platform::open_file(file_, "rb"));
    if (input.file == nullptr) {
        open_error_ = "the installer could not be read";
        set_error(error, open_error_);
        return false;
    }
    if (require_known_) {
        const bundles::Installer* known = bundles::find_installer(size);
        if (known == nullptr || known->kind != bundles::InstallerKind::macos) {
            open_error_ = "this file is not the recognised macOS installer";
            set_error(error, open_error_);
            return false;
        }
        say_line(news, "Checking the installer...");
        oa::base::sha256::Digest digest{};
        if (!hash_file(input.file, digest)) {
            open_error_ = "the installer could not be read";
            set_error(error, open_error_);
            return false;
        }
        if (!bundles::known_installer_hash(*known, digest)) {
            open_error_ = "this file is not the recognised macOS installer";
            set_error(error, open_error_);
            return false;
        }
    }
    uint8_t header[28];
    if (!seek_file(input.file, 0) || !read_full(input.file, header, sizeof header)) {
        open_error_ = "the installer could not be read";
        set_error(error, open_error_);
        return false;
    }
    oa::formats::xar::Archive archive;
    oa::formats::xar::Fault fault = oa::formats::xar::Fault::ok;
    if (!oa::formats::xar::read_header(header, archive, fault)) {
        open_error_ = "the installer could not be read";
        set_error(error, open_error_);
        return false;
    }
    if (archive.toc_compressed_bytes >
        static_cast<uint64_t>(std::numeric_limits<std::size_t>::max())) {
        open_error_ = "the installer could not be read";
        set_error(error, open_error_);
        return false;
    }
    std::vector<uint8_t> toc(static_cast<std::size_t>(archive.toc_compressed_bytes));
    if (!seek_file(input.file, archive.header_bytes) ||
        !read_full(input.file, toc.data(), toc.size()) ||
        !oa::formats::xar::read_toc(toc, archive.toc_bytes, archive.entries, fault)) {
        open_error_ = "the installer could not be read";
        set_error(error, open_error_);
        return false;
    }
    const oa::formats::xar::Entry* scripts =
        oa::formats::xar::find_entry(archive.entries, "package.pkg/Scripts");
    if (scripts == nullptr || scripts->directory ||
        scripts->encoding == oa::formats::xar::Encoding::unsupported) {
        open_error_ = "the installer could not be read";
        set_error(error, open_error_);
        return false;
    }
    if (scripts->heap_offset > std::numeric_limits<uint64_t>::max() - archive.heap_start) {
        open_error_ = "the installer could not be read";
        set_error(error, open_error_);
        return false;
    }
    scripts_at_ = archive.heap_start + scripts->heap_offset;
    scripts_bytes_ = scripts->stored_bytes;
    outer_gzip_ = scripts->encoding == oa::formats::xar::Encoding::gzip;
    if (scripts_at_ > size || scripts_bytes_ > size - scripts_at_) {
        open_error_ = "the installer could not be read";
        set_error(error, open_error_);
        return false;
    }
    ready_ = true;
    return true;
}

bool MacosInstallerSource::open(std::string* error) {
    return ensure(error, nullptr);
}

bool MacosInstallerSource::list(std::vector<ContentSourceFile>& files, std::string* error) {
    files.clear();
    if (!ensure(error, nullptr))
        return false;
    Pump pump;
    pump.job = Job::list;
    pump.listed = &files;
    pump.error = error;
    return InstallerWalk::run(*this, pump);
}

bool MacosInstallerSource::stream_file(
    const StreamRequest& request, StreamResult& result, std::string* error
) {
    result = {};
    if (!ensure(error, nullptr))
        return false;
    Pump pump;
    pump.job = Job::stream;
    pump.stream_request = &request;
    pump.stream_result = &result;
    pump.error = error;
    pump.chunk = chunk_size(request.chunk_bytes);
    if (!InstallerWalk::run(*this, pump)) {
        if (pump.halt == Halt::stop) {
            result.stopped = true;
            result.bytes = pump.active.targets.empty() ? 0 : pump.active.targets.front().filled;
            return true;
        }
        result.no_space = pump.no_space;
        return false;
    }
    return true;
}

bool MacosInstallerSource::copy_together(const CopyRequest& request, CopyResult& result) {
    result = {};
    try {
        std::string problem;
        if (!ensure(&problem, request.news)) {
            result.status = CopyStatus::failed;
            result.error = player_error(problem.empty() ? open_error_ : problem);
            return true;
        }
        Pump pump;
        pump.job = Job::copy;
        pump.request = &request;
        pump.result = &result;
        pump.error = &problem;
        pump.chunk = chunk_size(request.chunk_bytes);
        if (!InstallerWalk::run(*this, pump)) {
            if (pump.no_space || problem == "short space") {
                result.status = CopyStatus::no_space;
                result.error = "There is not enough free space to copy the game into Open "
                               "Annihilation. Free some space and start the game again.";
                return true;
            }
            if (pump.halt == Halt::stop) {
                result.status = CopyStatus::stopped;
                result.error = "The copy into Open Annihilation stopped. Start the game again to "
                               "continue it.";
                return true;
            }
            result.status = CopyStatus::failed;
            result.error =
                player_error(problem.empty() ? "the installer could not be read" : problem);
            return true;
        }
        if (pump.told)
            say_line(request.news, "Copied the game into Open Annihilation.");
        result.status = CopyStatus::finished;
        return true;
    } catch (const std::exception& failure) {
        result.status = CopyStatus::failed;
        result.error = player_error(failure.what());
        return true;
    }
}

int MacosInstallerSource::write_check(std::ostream& out) {
    std::string problem;
    if (!ensure(&problem, nullptr)) {
        out << sentence(problem.empty() ? open_error_ : problem) << '\n';
        out << "installer check: FAILED\n";
        return 1;
    }
    Pump pump;
    pump.job = Job::check;
    pump.error = &problem;
    if (!InstallerWalk::run(*this, pump)) {
        out << sentence(problem.empty() ? "the installer could not be read" : problem) << '\n';
        out << "installer check: FAILED\n";
        return 1;
    }
    // The same report as the other installers: each bundle with at least one
    // file in the installer, then each of its files; a bundle with none is
    // left out.
    out << "macOS installer: " << path_to_utf8(file_) << '\n';
    bool passed = true;
    bool any = false;
    for (std::size_t first = 0; first < pump.slots.size();) {
        const bundles::Bundle* bundle = pump.slots[first].bundle;
        std::size_t end = first;
        bool held = false;
        while (end < pump.slots.size() && pump.slots[end].bundle == bundle) {
            held = held || (pump.slots[end].seen && pump.slots[end].have_digest);
            ++end;
        }
        if (held) {
            any = true;
            out << "bundle " << bundle->id << '\n';
            for (std::size_t index = first; index < end; ++index) {
                const Slot& slot = pump.slots[index];
                out << "  " << slot.file->copy_name;
                if (!slot.seen || !slot.have_digest) {
                    out << " missing\n";
                    passed = false;
                    continue;
                }
                const auto hex = oa::base::sha256::to_hex(slot.digest);
                out << ' ';
                out.write(hex.data(), static_cast<std::streamsize>(hex.size()));
                const bool known = bundles::known_hash(*slot.file, slot.digest);
                if (known && !slot.filled_from.empty())
                    out << " ok (from " << slot.filled_from << ")\n";
                else
                    out << (known ? " ok\n" : " unknown\n");
                passed = passed && known;
            }
        }
        first = end;
    }
    if (!any || !passed) {
        out << "installer check: FAILED\n";
        return 1;
    }
    out << "installer check: passed\n";
    return 0;
}

int report_macos_installer(
    const fs::path& file,
    std::span<const bundles::Bundle> bundles,
    bool require_known,
    std::ostream& out
) {
    MacosInstallerSource source(file, bundles, require_known);
    return source.write_check(out);
}

int run_macos_installer_check(const fs::path& file) {
    return report_macos_installer(file, bundles::bundles(), true, std::cout);
}

} // namespace oa::app
