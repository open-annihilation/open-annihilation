// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace oa::app::full {

/// Keeps exact polygon-shape results in bounded, independently replaced sets.
///
/// Result supplies bytes(), the capacity of its owned buffers in bytes.
template <class Result>
class PlaneOrderCache {
  public:

    static constexpr size_t entry_limit = 4096;
    static constexpr size_t ways = 8;
    static constexpr size_t byte_limit = 32 * 1024 * 1024;
    static constexpr size_t scratch_byte_limit = 256 * 1024;

    /// Allocates the fixed entry registry before emission begins.
    PlaneOrderCache() : entries_(entry_limit) {}

    /// Returns the reusable normalized shape being looked up.
    /// @return polygon sizes and relative corner positions and depths
    [[nodiscard]] std::vector<int64_t>& shape() noexcept { return shape_; }

    /// Returns a result only when its complete normalized shape is equal.
    /// @param digest the shape's lookup digest
    /// @return the stored result, or null on a miss
    [[nodiscard]] const Result* find(uint64_t digest) noexcept {
        const auto first = set_begin(digest);
        for (size_t at = first; at < first + ways; ++at) {
            auto& entry = entries_[at];
            if (entry.used && entry.digest == digest && entry.shape == shape_) {
                entry.used = ++clock_;
                ++hits_;
                return &entry.result;
            }
        }
        return nullptr;
    }

    /// Returns reusable storage for a result computed on a cache miss.
    /// @return the result to replace completely
    [[nodiscard]] Result& scratch() noexcept { return scratch_; }

    /// Publishes a complete result without exceeding the entry or byte limits.
    /// @param digest the normalized shape's lookup digest
    /// @return the published result, or the transient result when too large to keep
    [[nodiscard]] const Result& publish(uint64_t digest) {
        ++builds_;
        const size_t bytes = shape_.capacity() * sizeof(int64_t) + scratch_.bytes();
        if (bytes > byte_limit)
            return scratch_;
        const auto first = set_begin(digest);
        size_t victim = first;
        for (size_t at = first; at < first + ways; ++at)
            if (entries_[at].used < entries_[victim].used)
                victim = at;
        auto& entry = entries_[victim];
        if (entry.used) {
            kept_bytes_ -= entry.bytes;
            --kept_entries_;
            entry.used = 0;
            entry.bytes = 0;
        }
        while (kept_bytes_ > byte_limit - bytes) {
            release(eviction_cursor_);
            eviction_cursor_ = (eviction_cursor_ + 1) % entry_limit;
        }
        entry.digest = digest;
        entry.used = ++clock_;
        entry.bytes = bytes;
        entry.shape.swap(shape_);
        std::swap(entry.result, scratch_);
        kept_bytes_ += bytes;
        ++kept_entries_;
        return entry.result;
    }

    /// Releases an unusually large transient result after the draw consumes it.
    void trim_scratch() noexcept {
        if (shape_.capacity() * sizeof(int64_t) + scratch_.bytes() > scratch_byte_limit) {
            std::vector<int64_t>{}.swap(shape_);
            scratch_ = {};
        }
    }

    /// Forgets results after the source content or display palette changes.
    void clear() noexcept {
        for (size_t at = 0; at < entries_.size(); ++at)
            release(at);
        std::vector<int64_t>{}.swap(shape_);
        scratch_ = {};
    }

    /// Returns owned result and key capacity, excluding the fixed registry.
    /// @return retained bytes
    [[nodiscard]] size_t bytes() const noexcept { return kept_bytes_; }

    /// Returns the number of retained exact shapes.
    /// @return entries, at most entry_limit
    [[nodiscard]] size_t size() const noexcept { return kept_entries_; }

    /// Returns complete result computations since construction.
    /// @return cache misses whose results were published
    [[nodiscard]] uint64_t builds() const noexcept { return builds_; }

    /// Returns exact shape reuse since construction.
    /// @return successful lookups
    [[nodiscard]] uint64_t hits() const noexcept { return hits_; }

  private:

    struct Entry {
        uint64_t digest{}, used{};
        size_t bytes{};
        std::vector<int64_t> shape;
        Result result{};
    };

    /// Returns the first entry of a digest's fixed set.
    /// @param digest the lookup digest
    /// @return an aligned entry index
    [[nodiscard]] static size_t set_begin(uint64_t digest) noexcept {
        return (digest % (entry_limit / ways)) * ways;
    }

    /// Releases an occupied entry and its accounting once.
    /// @param at the entry index
    void release(size_t at) noexcept {
        auto& entry = entries_[at];
        if (!entry.used)
            return;
        kept_bytes_ -= entry.bytes;
        --kept_entries_;
        entry = {};
    }

    std::vector<Entry> entries_;
    std::vector<int64_t> shape_;
    Result scratch_;
    size_t kept_bytes_{}, kept_entries_{}, eviction_cursor_{};
    uint64_t clock_{}, builds_{}, hits_{};
};

} // namespace oa::app::full
