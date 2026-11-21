// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

// The Windows thread clock is read in benchmark_thread_cpu.cpp, so that no
// file including this header sees the Windows headers' macros.
#if !defined(_WIN32) && (defined(__unix__) || defined(__APPLE__))
#include <time.h>
#endif

namespace oa::app::benchmark_timing {

/// Names the current thread's CPU clock, without substituting another clock.
/// @return native API name, or unsupported when no thread CPU clock is compiled
inline constexpr std::string_view thread_cpu_clock_name() noexcept {
#if defined(_WIN32)
    return "GetThreadTimes";
#elif defined(CLOCK_THREAD_CPUTIME_ID)
    return "CLOCK_THREAD_CPUTIME_ID";
#else
    return "unsupported";
#endif
}

/// Reads kernel plus user CPU consumed by the calling benchmark thread only.
/// @return cumulative nanoseconds, or none when unavailable or outside the range
[[nodiscard]] std::optional<uint64_t> thread_cpu_nanoseconds() noexcept;

} // namespace oa::app::benchmark_timing
