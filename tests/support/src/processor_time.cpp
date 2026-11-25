// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The processor time a test's own work takes (oa/test/processor_time.hpp).
#include "oa/test/processor_time.hpp"

#include <cstdint>
#include <ctime>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace oa::test {

std::optional<double> processor_seconds() noexcept {
#if defined(_WIN32)
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
        return std::nullopt;
    constexpr unsigned word_bits = 32;
    constexpr double ticks_per_second = 10000000.0;
    const auto ticks = [](FILETIME value) {
        return (static_cast<uint64_t>(value.dwHighDateTime) << word_bits) |
               static_cast<uint64_t>(value.dwLowDateTime);
    };
    return static_cast<double>(ticks(kernel) + ticks(user)) / ticks_per_second;
#else
    const std::clock_t spent = std::clock();
    if (spent == static_cast<std::clock_t>(-1))
        return std::nullopt;
    return static_cast<double>(spent) / CLOCKS_PER_SEC;
#endif
}

WorkTimer::WorkTimer() noexcept
    : processor_started_(processor_seconds()), real_started_(std::chrono::steady_clock::now()) {
}

WorkTime WorkTimer::elapsed() const noexcept {
    WorkTime took;
    took.real = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - real_started_
    );
    const std::optional<double> processor_now = processor_seconds();
    if (processor_started_ && processor_now)
        took.processor_seconds = *processor_now - *processor_started_;
    return took;
}

} // namespace oa::test
