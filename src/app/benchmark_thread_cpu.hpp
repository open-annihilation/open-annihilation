// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__unix__) || defined(__APPLE__)
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
inline std::optional<uint64_t> thread_cpu_nanoseconds() noexcept {
    constexpr uint64_t maximum = std::numeric_limits<uint64_t>::max();
#if defined(_WIN32)
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user))
        return {};
    constexpr unsigned word_bits = 32;
    constexpr uint64_t nanoseconds_per_filetime_tick = 100;
    const auto ticks = [](FILETIME value) {
        return (static_cast<uint64_t>(value.dwHighDateTime) << word_bits) |
               static_cast<uint64_t>(value.dwLowDateTime);
    };
    const auto kernel_ticks = ticks(kernel), user_ticks = ticks(user);
    if (user_ticks > maximum - kernel_ticks ||
        kernel_ticks + user_ticks > maximum / nanoseconds_per_filetime_tick)
        return {};
    return (kernel_ticks + user_ticks) * nanoseconds_per_filetime_tick;
#elif defined(CLOCK_THREAD_CPUTIME_ID)
    timespec value{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) != 0 || value.tv_sec < 0 ||
        value.tv_nsec < 0)
        return {};
    constexpr uint64_t nanoseconds_per_second = 1'000'000'000;
    const auto seconds = static_cast<uint64_t>(value.tv_sec);
    const auto remainder = static_cast<uint64_t>(value.tv_nsec);
    if (remainder >= nanoseconds_per_second ||
        seconds > (maximum - remainder) / nanoseconds_per_second)
        return {};
    return seconds * nanoseconds_per_second + remainder;
#else
    (void)maximum;
    return {};
#endif
}

} // namespace oa::app::benchmark_timing
