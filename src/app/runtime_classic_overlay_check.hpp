// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>

namespace oa::app::classic_overlay_check {

/// Dedicated render-tier check observations; ordinary gameplay never enables these.
struct State {
    bool watching{};       ///< whether the native check records its own classic overlay work
    bool reference{};      ///< force the original loop in this check without changing match rules
    uint64_t eligible{};   ///< typed units passing the original visibility checks
    uint64_t skipped{};    ///< zero-countdown Dots units whose remaining overlay work was skipped
    uint64_t projected{};  ///< units reaching the original overlay position/projection path
    uint64_t countdowns{}; ///< nonzero self-destruct labels submitted for painting
};

/// Starts empty observations for a native render-tier frame comparison.
/// @param reference whether this dedicated check uses the original overlay path
void begin(bool reference = false) noexcept;

/// Stops observation and withdraws the check-only reference override.
void end() noexcept;

/// Returns this calling thread's retained native-check observations.
/// @return counters for the most recent explicitly observed frames
[[nodiscard]] State read() noexcept;

} // namespace oa::app::classic_overlay_check
