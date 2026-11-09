// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "oa/core/player.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace oa::sim::match_runtime {

inline constexpr size_t ai_strike_tasks_per_player = 2;
inline constexpr size_t ai_strike_target_count = OA_PLAYER_COUNT * ai_strike_tasks_per_player;
/// Minimum gap between early native strike dispatches on the owning machine.
/// Six simulation ticks are 200 ms at 30 ticks per second. This bounds dispatch
/// frequency; a dispatch may still order every member of a large squad.
inline constexpr uint32_t ai_retarget_cooldown_ticks = 6;
inline constexpr size_t ai_retarget_header_bytes = 24;
inline constexpr size_t ai_retarget_target_bytes = 20;
inline constexpr size_t ai_retarget_state_bytes =
    ai_retarget_header_bytes + ai_strike_target_count * ai_retarget_target_bytes;

/// Distinguishes a live target binding from a loss that survives slot reuse.
enum class AiStrikeBinding : uint8_t { empty, bound, lost };

/// Keeps the target of one built-in land or naval strike task.
struct AiStrikeTarget {
    uint16_t slot{};
    uint8_t owner{};
    AiStrikeBinding binding{};
    uint32_t last_issue_tick{};
    uint32_t retry_not_before{};
    uint32_t native_next_tick{};
    bool native_clock_valid{};
    bool native_attacking{};
};

/// Limits early strike refreshes on the owning machine and retains target loss.
///
/// The save-only state includes native strike clocks. A restored clock applies
/// after the controller is initialized, before its first built-in dispatch.
struct AiRetargetState {
    std::array<AiStrikeTarget, ai_strike_target_count> targets{};
    uint32_t next_early_tick{};
    uint32_t prepared_tick{};
    uint8_t selection_cursor{};
    bool prepared_valid{};
    uint16_t builtin_players{};
    // A controller call owns this scope; it is never part of saved state.
    bool builtin_dispatch{};
    uint8_t builtin_player{};
    std::array<bool, ai_strike_target_count> restore_native_clock{};
    std::array<uint8_t, ai_retarget_state_bytes> encoded{};

    /// Marks every binding to a disappearing target before its slot can be reused.
    ///
    /// @param slot the disappearing unit slot
    void lose_target(uint16_t slot) noexcept;

    /// Encodes the bounded scheduler and target bindings without runtime pointers.
    ///
    /// @param unit_count the match's unit slot count, including slot zero
    /// @return the fixed-size little-endian state
    [[nodiscard]] std::span<const uint8_t> state_bytes(size_t unit_count) noexcept;

    /// Restores a complete scheduler, leaving it unchanged on malformed input.
    ///
    /// @param bytes the fixed-size little-endian state
    /// @param unit_count the destination match's unit slot count
    /// @return true when every field and binding is valid
    [[nodiscard]] bool restore(std::span<const uint8_t> bytes, size_t unit_count) noexcept;
};

} // namespace oa::sim::match_runtime
