// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace oa::sim::match_runtime {

/// Number of fixed physical-footprint results retained within one path-search pass.
inline constexpr size_t static_passability_entry_count = 32'768;
/// Maximum scratch storage retained by one match's physical memo.
inline constexpr size_t static_passability_storage_limit = 1'048'576;

/// A physical result for one movement class and footprint-origin cell.
struct StaticPassabilityEntry {
    uint32_t epoch{};
    uint32_t movement_class{};
    int32_t x{};
    int32_t z{};
    uint8_t value{};
};

/// Advances the invocation epoch, clearing stamps before an epoch can be reused.
///
/// @param[in,out] epoch the current invocation; zero means no invocation has begun
/// @param[in,out] entries fixed memo storage
inline void advance_static_passability_epoch(
    uint32_t& epoch, std::span<StaticPassabilityEntry> entries
) noexcept {
    if (++epoch != 0)
        return;
    for (auto& entry : entries)
        entry.epoch = 0;
    epoch = 1;
}

/// Reuses physical footprint checks only within one path-search invocation.
///
/// A caller checks map bounds and each player's current fog before looking up
/// a result. Class records and the physical map must remain unchanged until
/// the next begin_pass. Entries are derived scratch, never saved or hashed.
/// Construction prepares all storage; lookup, insertion and reset allocate nothing.
class StaticPassabilityMemo {
  public:

    /// Prepares empty fixed-capacity storage before simulation ticks.
    StaticPassabilityMemo() noexcept = default;

    /// Starts a fresh invocation, including another invocation at the same game tick.
    void begin_pass() noexcept { advance_static_passability_epoch(epoch_, entries_); }

    /// Reads a physical result only when its complete key belongs to this invocation.
    ///
    /// @param movement_class stable movement-class table index
    /// @param x footprint-origin column
    /// @param z footprint-origin row
    /// @param[out] value zero blocked or one passable; unchanged on a miss
    /// @return whether the current invocation contains this exact key
    [[nodiscard]] bool
    find(uint32_t movement_class, int32_t x, int32_t z, uint8_t& value) const noexcept {
        const auto& entry = entries_[slot(movement_class, x, z)];
        if (epoch_ == 0 || entry.epoch != epoch_ || entry.movement_class != movement_class ||
            entry.x != x || entry.z != z)
            return false;
        value = entry.value;
        return true;
    }

    /// Retains a physical result, replacing a colliding key without aliasing its value.
    ///
    /// @param movement_class stable movement-class table index
    /// @param x footprint-origin column
    /// @param z footprint-origin row
    /// @param value zero blocked or one passable; unknown-fog and other values are ignored
    void remember(uint32_t movement_class, int32_t x, int32_t z, uint8_t value) noexcept {
        if (epoch_ == 0 || value > 1)
            return;
        entries_[slot(movement_class, x, z)] = {epoch_, movement_class, x, z, value};
    }

    /// Locates the bounded storage slot for a complete physical key.
    ///
    /// @param movement_class stable movement-class table index
    /// @param x footprint-origin column
    /// @param z footprint-origin row
    /// @return a slot below static_passability_entry_count
    [[nodiscard]] static size_t slot(uint32_t movement_class, int32_t x, int32_t z) noexcept {
        constexpr uint32_t class_mixer = 0x9e3779b9u;
        constexpr uint32_t column_mixer = 0x85ebca6bu;
        constexpr uint32_t row_mixer = 0xc2b2ae35u;
        uint32_t hash = movement_class * class_mixer;
        hash ^= static_cast<uint32_t>(x) * column_mixer;
        hash ^= static_cast<uint32_t>(z) * row_mixer;
        hash ^= hash >> 16;
        return hash & (static_passability_entry_count - 1);
    }

  private:

    std::array<StaticPassabilityEntry, static_passability_entry_count> entries_{};
    uint32_t epoch_{};
};

static_assert((static_passability_entry_count & (static_passability_entry_count - 1)) == 0);
static_assert(sizeof(StaticPassabilityMemo) <= static_passability_storage_limit);

} // namespace oa::sim::match_runtime
