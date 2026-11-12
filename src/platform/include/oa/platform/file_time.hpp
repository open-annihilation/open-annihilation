// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A file's time as whole seconds since 1970, and back, without the time zone database.
//
// A standard library whose file clock converts to and from the system clock (`to_sys` and
// `from_sys`) is used that way. The Visual C++ file clock has neither: its own way goes through
// UTC, `to_utc` and then `utc_clock::to_sys` (back, `utc_clock::from_sys` and then `from_utc`),
// which loads the time zone database for the leap seconds, and where that database is missing,
// as under Wine, the process aborts. That clock counts 100 ns ticks from 1601-01-01, as a
// Windows file time does, which is 11644473600 seconds before 1970; the leap seconds its
// conversion to UTC adds, the conversion to the system clock takes away again, so the seconds
// since 1970 are its count less that difference, the same result without the database.
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <ratio>

namespace oa::platform {

/// A tick of a file clock that has no `to_sys`: 100 ns, as a Windows file time counts.
using FileTimeTick = std::chrono::duration<long long, std::ratio<1, 10'000'000>>;

/// The time from such a clock's epoch, 1601-01-01, to 1970-01-01: 11644473600 seconds.
inline constexpr FileTimeTick file_epoch_to_1970{0x19DB1DED53E8000LL};

/// Converts a time of a file clock to whole seconds since 1970 (UTC), rounded down, so a time
/// before 1970 that is not a whole second gives the second before it.
///
/// A clock with `to_sys` converts through it. A clock without it has to count 100 ns ticks from
/// 1601-01-01, as the Visual C++ file clock does; this does not compile for any other.
///
/// @tparam Clock the file clock
/// @param time a time of the clock
/// @return seconds since 1970
template <typename Clock>
[[nodiscard]] int64_t clock_seconds_since_1970(typename Clock::time_point time) noexcept {
    if constexpr (requires { Clock::to_sys(time); }) {
        return static_cast<int64_t>(
            std::chrono::floor<std::chrono::seconds>(Clock::to_sys(time)).time_since_epoch().count()
        );
    } else {
        static_assert(std::ratio_equal_v<typename Clock::period, FileTimeTick::period>);
        const auto since_1970 = time.time_since_epoch() - file_epoch_to_1970;
        return static_cast<int64_t>(std::chrono::floor<std::chrono::seconds>(since_1970).count());
    }
}

/// Converts whole seconds since 1970 (UTC) to a time of a file clock, the time that
/// clock_seconds_since_1970() reads back as the same seconds.
///
/// A clock with `from_sys` converts through it. A clock without it has to count 100 ns ticks
/// from 1601-01-01, as the Visual C++ file clock does; this does not compile for any other.
///
/// @tparam Clock the file clock
/// @param seconds seconds since 1970
/// @return the clock's time
template <typename Clock>
[[nodiscard]] typename Clock::time_point clock_time_from_seconds(int64_t seconds) noexcept {
    const std::chrono::sys_seconds system{std::chrono::seconds{seconds}};
    if constexpr (requires { Clock::from_sys(system); }) {
        return std::chrono::time_point_cast<typename Clock::duration>(Clock::from_sys(system));
    } else {
        static_assert(std::ratio_equal_v<typename Clock::period, FileTimeTick::period>);
        const FileTimeTick since_epoch =
            std::chrono::duration_cast<FileTimeTick>(system.time_since_epoch()) +
            file_epoch_to_1970;
        return typename Clock::time_point{
            std::chrono::duration_cast<typename Clock::duration>(since_epoch)
        };
    }
}

/// Converts a file's time, as std::filesystem gives it, to whole seconds since 1970 (UTC),
/// rounded down.
///
/// @param time the file time
/// @return seconds since 1970
[[nodiscard]] int64_t file_seconds_since_1970(std::filesystem::file_time_type time) noexcept;

/// Converts whole seconds since 1970 (UTC) to a file time for std::filesystem, such as a time to
/// give a file; file_seconds_since_1970() reads it back as the same seconds.
///
/// @param seconds seconds since 1970
/// @return the file time
[[nodiscard]] std::filesystem::file_time_type file_time_from_seconds(int64_t seconds) noexcept;

} // namespace oa::platform
