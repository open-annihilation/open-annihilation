// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>

namespace oa::sim::match_runtime {

/// Indexes owned orders and certificates of their observer-list membership.
///
/// Keys identify only their own records. No simulation operation walks the
/// hash slots in their address-dependent order. Construction allocates; all
/// later operations are bounded and allocate nothing. Capacity exhaustion
/// leaves a record unindexed so the caller can retain the full legacy checks.
class OrderOwnershipIndex {
  public:

    static constexpr uint32_t no_owner = std::numeric_limits<uint32_t>::max();
    static constexpr uint32_t no_observer = std::numeric_limits<uint32_t>::max();
    static constexpr uint32_t uncertified_observer = no_observer - 1;

    struct Entry {
        const void* key{};
        uint32_t owner_index{no_owner};
        uint32_t observer_slot{uncertified_observer};
    };

    /// Allocates an index for the match's bounded unit pool.
    ///
    /// @param unit_slots unit slots including the reserved slot
    explicit OrderOwnershipIndex(size_t unit_slots) {
        constexpr size_t maximum_unit_slots = 32768;
        constexpr uint32_t minimum_capacity = 16;
        constexpr uint32_t entries_per_unit = 4;
        if (unit_slots == 0 || unit_slots > maximum_unit_slots)
            return;
        capacity_ = std::bit_ceil(
            std::max(minimum_capacity, static_cast<uint32_t>(unit_slots) * entries_per_unit)
        );
        shift_ = static_cast<uint32_t>(std::countr_zero(capacity_));
        entries_ = std::make_unique<Entry[]>(capacity_);
    }

    /// Finds the certificate of one order.
    ///
    /// @param key address of its canonical order
    /// @return its entry, or null when it is not indexed
    Entry* find(const void* key) noexcept {
        if (!key || capacity_ == 0)
            return nullptr;
        uint32_t position = bucket(key);
        for (uint32_t probed = 0; probed < capacity_; ++probed) {
            auto& entry = entries_[position];
            if (entry.key == key)
                return &entry;
            if (entry.key == nullptr)
                return nullptr;
            position = next(position);
        }
        return nullptr;
    }

    /// Finds the certificate of one order without changing it.
    ///
    /// @param key address of its canonical order
    /// @return its entry, or null when it is not indexed
    const Entry* find(const void* key) const noexcept {
        return const_cast<OrderOwnershipIndex*>(this)->find(key);
    }

    /// Certifies a newly constructed order before any list can contain it.
    ///
    /// @param key address of its canonical order
    /// @return false when the bounded index cannot retain the fresh record
    bool fresh(const void* key) noexcept {
        assert(find(key) == nullptr);
        auto* entry = ensure(key);
        if (!entry)
            return false;
        assert(entry->owner_index == no_owner);
        assert(entry->observer_slot == uncertified_observer);
        entry->observer_slot = no_observer;
        return true;
    }

    /// Records an order's current place in its owning vector.
    ///
    /// A previously unindexed record stays uncertified about observer lists.
    ///
    /// @param key address of its canonical order
    /// @param position current ownership-vector index
    /// @return false when the bounded index cannot retain the position
    bool remember_owner(const void* key, size_t position) noexcept {
        if (position >= no_owner)
            return false;
        auto* entry = ensure(key);
        if (!entry)
            return false;
        entry->owner_index = static_cast<uint32_t>(position);
        return true;
    }

    /// Removes an order's index entry before its record is destroyed.
    ///
    /// Backward shifting retains every remaining probe chain without tombstones.
    ///
    /// @param key address of its canonical order
    void forget(const void* key) noexcept {
        auto* entry = find(key);
        if (!entry)
            return;
        uint32_t hole = static_cast<uint32_t>(entry - entries_.get());
        uint32_t position = next(hole);
        while (entries_[position].key != nullptr) {
            const auto home = bucket(entries_[position].key);
            if (distance(home, hole) < distance(home, position)) {
                entries_[hole] = entries_[position];
                hole = position;
            }
            position = next(position);
        }
        entries_[hole] = {};
        --count_;
    }

    /// Starts an operation tick and requests at most one stable compaction.
    ///
    /// @param tick current simulation tick
    /// @return true when earlier ticks left holes to compact
    bool begin_tick(uint32_t tick) noexcept {
        if (tick_known_ && tick_ == tick)
            return false;
        tick_known_ = true;
        tick_ = tick;
        return holes_ != 0;
    }

    /// Records one immediately destroyed ownership slot.
    void removed() noexcept { ++holes_; }

    /// Records the stable removal of all empty ownership slots.
    void compacted() noexcept { holes_ = 0; }

    /// Returns the number of active records, retaining legacy list-walk bounds.
    ///
    /// @param slots current ownership-vector size including holes
    /// @return the number of nonempty ownership slots
    size_t active_count(size_t slots) const noexcept {
        assert(holes_ <= slots);
        return slots - holes_;
    }

    /// Returns the bounded entry storage size.
    /// @return the number of hash slots
    size_t capacity() const noexcept { return capacity_; }

  private:

    std::unique_ptr<Entry[]> entries_{};
    uint32_t capacity_{}, shift_{}, count_{};
    size_t holes_{};
    uint32_t tick_{};
    bool tick_known_{};

    /// Returns the address bucket used only to find this same record.
    /// @param key address of its canonical order
    /// @return the initial hash slot
    uint32_t bucket(const void* key) const noexcept {
        constexpr uint64_t golden_multiplier = 0x9e3779b97f4a7c15ull;
        const auto address = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(key));
        return static_cast<uint32_t>((address * golden_multiplier) >> (64 - shift_));
    }

    /// Advances one hash slot with wraparound.
    /// @param position current hash slot
    /// @return the next hash slot
    uint32_t next(uint32_t position) const noexcept { return (position + 1) & (capacity_ - 1); }

    /// Returns the forward circular distance between two hash slots.
    /// @param from first hash slot
    /// @param to second hash slot
    /// @return the forward distance with wraparound
    uint32_t distance(uint32_t from, uint32_t to) const noexcept {
        return (to - from) & (capacity_ - 1);
    }

    /// Finds or inserts an uncertified entry without growing the storage.
    /// @param key address of its canonical order
    /// @return its entry, or null when no insertion is possible
    Entry* ensure(const void* key) noexcept {
        if (auto* entry = find(key))
            return entry;
        if (!key || capacity_ == 0 || count_ >= capacity_ - capacity_ / 4)
            return nullptr;
        uint32_t position = bucket(key);
        while (entries_[position].key != nullptr)
            position = next(position);
        auto& entry = entries_[position];
        entry.key = key;
        ++count_;
        return &entry;
    }
};

} // namespace oa::sim::match_runtime
