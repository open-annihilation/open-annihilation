// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "oa/sim/model_runtime/instance.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>

namespace oa::app {

/// Reuses exact model-space transforms within one planning worker's frame.
/// Each retained instance owns its geometry; unit records and world origins
/// remain with the callers. A miss always leaves the ordinary rebuild in use.
class ModelTransformCache {
  public:

    static constexpr size_t entry_capacity = 64;
    static constexpr size_t storage_limit = 512U * 1024U;
    static constexpr size_t piece_limit = 64;
    static constexpr size_t vertex_limit = 2048;

    /// Counts the bounded lookups and copies of the current frame.
    struct Counts {
        uint64_t hits{}, misses{}, stores{}, overflows{};
    };

    /// Sets the admission limits, clamped to the cache's fixed capacities.
    /// @param entries the most exact poses retained in a frame
    /// @param bytes the most bytes of cache and owned transform storage
    explicit ModelTransformCache(size_t entries = entry_capacity, size_t bytes = storage_limit)
        : entry_limit_(std::min(entries, entry_capacity)),
          byte_limit_(std::min(bytes, storage_limit)) {}

    /// Begins a frame under the worker's program floating-point control.
    /// Owned buffers remain bounded and reusable; old poses are not looked up.
    void begin_frame() noexcept {
        table_.fill(0);
        used_ = 0;
        counts_ = {};
    }

    /// Copies matching rebuilt model-space geometry without allocating.
    /// @param[in,out] instance the unit's original or presentation instance
    /// @param root effective root rotation after the renderer's threshold
    /// @return true when the exact inputs matched retained rebuilt geometry
    [[nodiscard]] bool
    reuse(sim::model_runtime::Instance& instance, sim::model_runtime::RotationWords root) noexcept {
        if (!instance.model_handle() || instance.pieces().size() > piece_limit) {
            ++counts_.misses;
            return false;
        }
        const auto key = digest(instance, root);
        for (size_t probe = 0; probe < probe_limit; ++probe) {
            const auto at = (key + probe) & (table_capacity - 1);
            if (table_[at] == 0)
                break;
            const auto& entry = entries_[table_[at] - 1];
            if (entry.digest == key && instance.reuse_transforms(entry.instance, root)) {
                ++counts_.hits;
                return true;
            }
        }
        ++counts_.misses;
        return false;
    }

    /// Retains a completed ordinary rebuild for later exact matches.
    /// @param instance immutable geometry completed under this worker's control
    /// @param root effective root rotation used by that rebuild
    void
    remember(const sim::model_runtime::Instance& instance, sim::model_runtime::RotationWords root) {
        if (!instance.model_handle() || instance.pieces().size() > piece_limit ||
            instance.transforms_dirty())
            return;
        size_t vertices = 0;
        for (const auto& piece : instance.pieces()) {
            if (piece.transformed_vertices.size() > vertex_limit - vertices)
                return;
            vertices += piece.transformed_vertices.size();
        }
        const auto key = digest(instance, root);
        size_t place = table_capacity;
        for (size_t probe = 0; probe < probe_limit; ++probe) {
            const auto at = (key + probe) & (table_capacity - 1);
            if (table_[at] == 0) {
                place = at;
                break;
            }
            const auto& entry = entries_[table_[at] - 1];
            if (entry.digest == key && same_pose(entry.instance, instance) &&
                same_rotation(entry.root, root))
                return;
        }
        if (used_ == entry_limit_ || place == table_capacity || byte_limit_ < sizeof(*this)) {
            ++counts_.overflows;
            return;
        }
        auto& entry = entries_[used_];
        const bool same_shape = same_topology(entry.instance, instance);
        const auto source_bytes = instance.transform_storage_bytes();
        if (source_bytes > byte_limit_ - sizeof(*this)) {
            ++counts_.overflows;
            return;
        }
        const auto wanted = same_shape ? entry.bytes : source_bytes;
        if (!fit(wanted, used_)) {
            ++counts_.overflows;
            return;
        }
        stored_bytes_ -= entry.bytes;
        if (!same_shape)
            entry.instance = {};
        entry.instance = instance;
        entry.bytes = entry.instance.transform_storage_bytes();
        stored_bytes_ += entry.bytes;
        if (!fit(entry.bytes, used_)) {
            release(used_);
            ++counts_.overflows;
            return;
        }
        entry.root = root;
        entry.digest = key;
        table_[place] = static_cast<uint16_t>(++used_);
        ++counts_.stores;
    }

    /// Returns this frame's private diagnostic counters.
    /// @return exact reuse, fallback and admission counts
    [[nodiscard]] const Counts& counts() const noexcept { return counts_; }

    /// Returns the exact retained cache and dynamic transform buffer bytes.
    /// @return bytes held by this cache, excluding shared immutable assets
    [[nodiscard]] size_t storage_bytes() const noexcept { return sizeof(*this) + stored_bytes_; }

  private:

    static constexpr size_t table_capacity = entry_capacity * 2;
    static constexpr size_t probe_limit = 8;
    static constexpr uint64_t digest_prime = 0x9e3779b185ebca87ULL;

    struct Entry {
        sim::model_runtime::Instance instance{};
        sim::model_runtime::RotationWords root{};
        uint64_t digest{};
        size_t bytes{};
    };

    /// Compares all three exact root or local angle words.
    /// @param a first rotation
    /// @param b second rotation
    /// @return true when every word matches
    static bool same_rotation(
        sim::model_runtime::RotationWords a, sim::model_runtime::RotationWords b
    ) noexcept {
        return a.xy == b.xy && a.xz == b.xz && a.yz == b.yz;
    }

    /// Compares the immutable model and its instance's complete piece links.
    /// @param a first instance
    /// @param b second instance
    /// @return true when the object order and hierarchy match
    static bool same_topology(
        const sim::model_runtime::Instance& a, const sim::model_runtime::Instance& b
    ) noexcept {
        if (a.model_handle() != b.model_handle() || a.root_piece() != b.root_piece() ||
            a.pieces().size() != b.pieces().size())
            return false;
        for (size_t index = 0; index < a.pieces().size(); ++index) {
            const auto& first = a.pieces()[index];
            const auto& second = b.pieces()[index];
            if (first.object_index != second.object_index || first.parent != second.parent ||
                first.first_child != second.first_child ||
                first.next_sibling != second.next_sibling)
                return false;
        }
        return true;
    }

    /// Compares the model, hierarchy and all local transform inputs.
    /// @param a first instance
    /// @param b second instance
    /// @return true when every geometry input except the root rotation matches
    static bool same_pose(
        const sim::model_runtime::Instance& a, const sim::model_runtime::Instance& b
    ) noexcept {
        if (!same_topology(a, b))
            return false;
        for (size_t index = 0; index < a.pieces().size(); ++index) {
            const auto& first = a.pieces()[index];
            const auto& second = b.pieces()[index];
            if (first.translation.x != second.translation.x ||
                first.translation.y != second.translation.y ||
                first.translation.z != second.translation.z ||
                !same_rotation(first.rotation, second.rotation))
                return false;
        }
        return true;
    }

    /// Hashes exact geometry inputs for a bounded lookup, never for identity.
    /// @param instance local translations, rotations, hierarchy and model
    /// @param root effective root rotation
    /// @return lookup digest; a full comparison still decides each hit
    static uint64_t digest(
        const sim::model_runtime::Instance& instance, sim::model_runtime::RotationWords root
    ) noexcept {
        uint64_t result = std::hash<const void*>{}(instance.model_handle().get());
        const auto add = [&](uint64_t word) { result = (result ^ word) * digest_prime; };
        const auto pair = [](uint32_t low, uint32_t high) {
            return uint64_t{low} | (uint64_t{high} << 32);
        };
        add(uint16_t(root.xy) | (uint64_t{uint16_t(root.xz)} << 16) |
            (uint64_t{uint16_t(root.yz)} << 32));
        add(pair(instance.root_piece(), static_cast<uint32_t>(instance.pieces().size())));
        for (const auto& piece : instance.pieces()) {
            add(pair(uint32_t(piece.translation.x), uint32_t(piece.translation.y)));
            add(pair(uint32_t(piece.translation.z), uint16_t(piece.rotation.xy)));
            add(uint16_t(piece.rotation.xz) | (uint64_t{uint16_t(piece.rotation.yz)} << 16));
            add(pair(piece.object_index, piece.parent));
            add(pair(piece.first_child, piece.next_sibling));
        }
        return result;
    }

    /// Releases one inactive slot's owned geometry buffers.
    /// @param index slot that is not registered in the current frame's table
    void release(size_t index) noexcept {
        auto& entry = entries_[index];
        stored_bytes_ -= entry.bytes;
        entry.instance = {};
        entry.bytes = 0;
    }

    /// Makes room by releasing only prior-frame, inactive tail slots.
    /// @param wanted replacement slot's required dynamic storage bytes
    /// @param replace slot about to be replaced
    /// @return true when the retained storage stays inside its byte limit
    bool fit(size_t wanted, size_t replace) noexcept {
        const size_t room = byte_limit_ - sizeof(*this);
        for (size_t index = entry_capacity;
             index > used_ + 1 &&
             (wanted > room || stored_bytes_ - entries_[replace].bytes > room - wanted);)
            release(--index);
        return wanted <= room && stored_bytes_ - entries_[replace].bytes <= room - wanted;
    }

    std::array<Entry, entry_capacity> entries_{};
    std::array<uint16_t, table_capacity> table_{};
    size_t entry_limit_{}, byte_limit_{}, used_{}, stored_bytes_{};
    Counts counts_{};
};

static_assert(sizeof(ModelTransformCache) < ModelTransformCache::storage_limit);

} // namespace oa::app
