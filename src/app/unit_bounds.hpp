// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "oa/core/types.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace oa::app {

/// The current frame's unit bounds, indexed by the units' 16-bit slots.
/// Slots retain their storage between frames; absent units have no bounds.
class UnitBounds {
  public:

    /// The complete range of 16-bit unit slots, including the unused slot zero.
    static constexpr size_t max_slots = size_t{std::numeric_limits<uint16_t>::max()} + 1;

    /// Starts a frame with no unit bounds, retaining the slots' storage.
    /// A wrapped frame stamp clears all old stamps before any slot is reused.
    void begin_frame() noexcept {
        if (frame_ == std::numeric_limits<uint16_t>::max()) {
            for (auto& entry : entries_)
                entry.frame = 0;
            frame_ = 1;
        } else {
            ++frame_;
        }
    }

    /// Returns a unit's bounds for accumulation, initially an empty rectangle.
    ///
    /// @param id the unit slot
    /// @return the bounds retained for this frame; later slot growth may move it
    [[nodiscard]] oa::Rect32& touch(uint16_t id) {
        const size_t needed = size_t{id} + 1;
        if (needed > entries_.capacity())
            entries_.reserve(std::min(max_slots, std::max(needed, entries_.capacity() * 2)));
        if (needed > entries_.size())
            entries_.resize(needed);
        auto& entry = entries_[id];
        if (entry.frame != frame_) {
            entry.bounds = {
                std::numeric_limits<int32_t>::max(),
                std::numeric_limits<int32_t>::max(),
                std::numeric_limits<int32_t>::min(),
                std::numeric_limits<int32_t>::min()
            };
            entry.frame = frame_;
        }
        return entry.bounds;
    }

    /// Returns a unit's bounds only when this frame accumulated them.
    ///
    /// @param id the unit slot
    /// @return the current bounds, or null for an absent slot
    [[nodiscard]] const oa::Rect32* find(uint16_t id) const noexcept {
        return id < entries_.size() && entries_[id].frame == frame_ ? &entries_[id].bounds
                                                                    : nullptr;
    }

    /// Returns the retained slot capacity, bounded by max_slots.
    ///
    /// @return the allocated slots, including unused slots below the largest ID
    [[nodiscard]] size_t storage_slots() const noexcept { return entries_.capacity(); }

  private:

    struct Entry {
        oa::Rect32 bounds{};
        uint16_t frame{};
    };

    std::vector<Entry> entries_{};
    uint16_t frame_{1};
};

} // namespace oa::app
