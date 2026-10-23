// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Files a sandbox can read without a directory. Matching is byte for byte,
// never through a file system's case folding.

#include "oa/data/script_host/memory_sources.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace oa::data::script_host {

bool MemorySources::path_before(const Stored& stored, std::string_view path) {
    return std::string_view(stored.path) < path;
}

MemorySources::MemorySources(MemorySources&& other) noexcept
    : files_(std::move(other.files_)), total_bytes_(other.total_bytes_) {
    other.total_bytes_ = 0;
}

MemorySources& MemorySources::operator=(MemorySources&& other) noexcept {
    if (this != &other) {
        files_ = std::move(other.files_);
        total_bytes_ = other.total_bytes_;
        other.total_bytes_ = 0;
    }
    return *this;
}

bool MemorySources::add(std::string_view path, std::span<const uint8_t> bytes) noexcept {
    if (path.empty())
        return false;
    try {
        std::string path_copy(path);
        std::vector<uint8_t> copy(bytes.begin(), bytes.end());
        const std::size_t added = copy.size();
        const auto found = std::lower_bound(files_.begin(), files_.end(), path, path_before);
        if (found != files_.end() && std::string_view(found->path) == path) {
            const std::size_t previous = found->bytes.size();
            if (added > previous &&
                added - previous > std::numeric_limits<std::size_t>::max() - total_bytes_)
                return false;
            found->bytes = std::move(copy);
            total_bytes_ = total_bytes_ - previous + added;
            return true;
        }
        if (added > std::numeric_limits<std::size_t>::max() - total_bytes_)
            return false;
        files_.insert(found, Stored{std::move(path_copy), std::move(copy)});
        total_bytes_ += added;
        return true;
    } catch (...) {
        return false;
    }
}

std::span<const uint8_t> MemorySources::find(std::string_view path) const noexcept {
    const auto found = locate(path);
    if (found == files_.end())
        return {};
    return std::span<const uint8_t>(found->bytes.data(), found->bytes.size());
}

std::size_t MemorySources::size() const noexcept {
    return files_.size();
}

std::size_t MemorySources::total_bytes() const noexcept {
    return total_bytes_;
}

SourceHooks MemorySources::hooks() const noexcept {
    SourceHooks reader;
    reader.context = const_cast<MemorySources*>(this);
    reader.read = &MemorySources::read_file;
    return reader;
}

bool MemorySources::read_file(void* context, std::string_view path, std::vector<uint8_t>& bytes) {
    bytes.clear();
    if (context == nullptr)
        return false;
    const auto* sources = static_cast<const MemorySources*>(context);
    try {
        const auto found = sources->locate(path);
        if (found == sources->files_.end())
            return false;
        bytes.assign(found->bytes.begin(), found->bytes.end());
        return true;
    } catch (...) {
        bytes.clear();
        return false;
    }
}

std::vector<MemorySources::Stored>::const_iterator
MemorySources::locate(std::string_view path) const {
    const auto found = std::lower_bound(files_.begin(), files_.end(), path, path_before);
    if (found == files_.end() || std::string_view(found->path) != path)
        return files_.end();
    return found;
}

} // namespace oa::data::script_host
