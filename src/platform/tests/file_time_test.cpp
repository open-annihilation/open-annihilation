// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A file's time as whole seconds since 1970 and back (oa/platform/file_time.hpp): 1970-01-01,
// 2026-11-12, a time before 1970, times between two seconds rounded down and round trips, through
// a clock with to_sys (nanoseconds from 2174, as GCC's library counts) and one of the Visual C++
// shape (100 ns ticks from 1601 and no to_sys); then this system's own file clock, beside the
// system clock, and a file whose time is set and read back.

#include "oa/platform/file_time.hpp"

#include "oa/test/check.hpp"
#include "oa/test/scratch_directory.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ratio>
#include <system_error>
#include <type_traits>

namespace {

namespace fs = std::filesystem;
using oa::platform::clock_seconds_since_1970;
using oa::platform::clock_time_from_seconds;
using oa::platform::file_seconds_since_1970;
using oa::platform::file_time_from_seconds;

/// 2026-11-12T00:00:00Z, in seconds since 1970.
constexpr int64_t november_2026 = 1'794'441'600;
/// 1969-07-20T20:17:40Z, in seconds since 1970: a time before 1970.
constexpr int64_t july_1969 = -14'182'940;
/// 1601-01-01T00:00:00Z, in seconds since 1970: the Windows file time's epoch.
constexpr int64_t january_1601 = -11'644'473'600;

/// Whether a clock converts to the system clock itself; the Visual C++ file clock does not.
template <typename Clock>
concept ConvertsToSys = requires(typename Clock::time_point time) { Clock::to_sys(time); };

/// The system clock's time, in whole seconds since 1970.
///
/// @return the seconds
int64_t system_seconds() {
    return static_cast<int64_t>(
        std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now())
            .time_since_epoch()
            .count()
    );
}

/// A file clock of the Visual C++ shape: 100 ns ticks from 1601-01-01, and no to_sys or
/// from_sys.
struct TicksFrom1601Clock {
    using rep = long long;
    using period = std::ratio<1, 10'000'000>;
    using duration = std::chrono::duration<rep, period>;
    using time_point = std::chrono::time_point<TicksFrom1601Clock>;
};

/// A file clock with to_sys and from_sys: nanoseconds from 2174-01-01, as GCC's library counts.
struct NanosecondsFrom2174Clock {
    using rep = long long;
    using period = std::nano;
    using duration = std::chrono::duration<rep, period>;
    using time_point = std::chrono::time_point<NanosecondsFrom2174Clock>;

    /// 2174-01-01 in seconds since 1970.
    static constexpr std::chrono::seconds epoch_after_1970{6'437'664'000};

    /// Converts a time of this clock to the system clock.
    ///
    /// @param time the time
    /// @return the system clock's time
    template <typename Duration>
    static std::chrono::sys_time<std::common_type_t<Duration, std::chrono::seconds>>
    to_sys(const std::chrono::time_point<NanosecondsFrom2174Clock, Duration>& time) {
        return std::chrono::sys_time<std::common_type_t<Duration, std::chrono::seconds>>{
            time.time_since_epoch() + epoch_after_1970
        };
    }

    /// Converts a time of the system clock to this clock.
    ///
    /// @param time the system clock's time
    /// @return this clock's time
    template <typename Duration>
    static std::chrono::
        time_point<NanosecondsFrom2174Clock, std::common_type_t<Duration, std::chrono::seconds>>
        from_sys(const std::chrono::sys_time<Duration>& time) {
        return std::chrono::time_point<
            NanosecondsFrom2174Clock,
            std::common_type_t<Duration, std::chrono::seconds>>{
            time.time_since_epoch() - epoch_after_1970
        };
    }
};

static_assert(!ConvertsToSys<TicksFrom1601Clock>);
static_assert(ConvertsToSys<NanosecondsFrom2174Clock>);

/// A time of a clock that counts 100 ns ticks from 1601-01-01, made from its count.
///
/// @param seconds whole seconds since 1970
/// @param ticks 100 ns ticks after them
/// @return the clock's time
template <typename Clock>
typename Clock::time_point ticks_from_1601(int64_t seconds, long long ticks = 0) {
    return typename Clock::time_point{
        typename Clock::duration{(seconds - january_1601) * 10'000'000LL + ticks}
    };
}

/// A time of NanosecondsFrom2174Clock, made from its count.
///
/// @param seconds whole seconds since 1970
/// @param nanoseconds nanoseconds after them
/// @return the clock's time
NanosecondsFrom2174Clock::time_point
nanoseconds_from_2174(int64_t seconds, long long nanoseconds = 0) {
    return NanosecondsFrom2174Clock::time_point{NanosecondsFrom2174Clock::duration{
        (seconds - NanosecondsFrom2174Clock::epoch_after_1970.count()) * 1'000'000'000LL +
        nanoseconds
    }};
}

/// Checks a clock of the Visual C++ shape against times made from its count of ticks.
///
/// @tparam Clock the clock
template <typename Clock>
void ticks_from_1601_convert() {
    const auto seconds = [](typename Clock::time_point time) {
        return clock_seconds_since_1970<Clock>(time);
    };
    OA_CHECK(seconds(ticks_from_1601<Clock>(0)) == 0);
    OA_CHECK(seconds(ticks_from_1601<Clock>(november_2026)) == november_2026);
    OA_CHECK(seconds(ticks_from_1601<Clock>(july_1969)) == july_1969);
    // The clock's epoch, and its count read directly.
    OA_CHECK(seconds(typename Clock::time_point{}) == january_1601);
    OA_CHECK(
        seconds(typename Clock::time_point{typename Clock::duration{134'389'152'000'000'000LL}}) ==
        november_2026
    );
    // Between two seconds, the earlier one: before 1970 too.
    OA_CHECK(seconds(ticks_from_1601<Clock>(0, -1)) == -1);
    OA_CHECK(seconds(ticks_from_1601<Clock>(july_1969, 5'000'000)) == july_1969);
    OA_CHECK(seconds(ticks_from_1601<Clock>(july_1969, -1)) == july_1969 - 1);
    OA_CHECK(seconds(ticks_from_1601<Clock>(november_2026, 9'999'999)) == november_2026);
    // Back from seconds: the same count, and the same seconds again.
    for (const int64_t known : {int64_t{0}, november_2026, july_1969, int64_t{-1}, january_1601}) {
        OA_CHECK(clock_time_from_seconds<Clock>(known) == ticks_from_1601<Clock>(known));
        OA_CHECK(seconds(clock_time_from_seconds<Clock>(known)) == known);
    }
}

/// The clock of the Visual C++ shape.
void a_clock_without_to_sys_counts_from_1601() {
    ticks_from_1601_convert<TicksFrom1601Clock>();
}

/// The clock with to_sys, against times made from its count.
void a_clock_with_to_sys_converts_through_it() {
    using Clock = NanosecondsFrom2174Clock;
    const auto seconds = [](Clock::time_point time) {
        return clock_seconds_since_1970<Clock>(time);
    };
    OA_CHECK(seconds(nanoseconds_from_2174(0)) == 0);
    OA_CHECK(seconds(nanoseconds_from_2174(november_2026)) == november_2026);
    OA_CHECK(seconds(nanoseconds_from_2174(july_1969)) == july_1969);
    OA_CHECK(seconds(Clock::time_point{}) == Clock::epoch_after_1970.count());
    OA_CHECK(seconds(nanoseconds_from_2174(0, -1)) == -1);
    OA_CHECK(seconds(nanoseconds_from_2174(july_1969, 500'000'000)) == july_1969);
    OA_CHECK(seconds(nanoseconds_from_2174(july_1969, -1)) == july_1969 - 1);
    OA_CHECK(seconds(nanoseconds_from_2174(november_2026, 999'999'999)) == november_2026);
    for (const int64_t known : {int64_t{0}, november_2026, july_1969, int64_t{-1}}) {
        OA_CHECK(clock_time_from_seconds<Clock>(known) == nanoseconds_from_2174(known));
        OA_CHECK(seconds(clock_time_from_seconds<Clock>(known)) == known);
    }
}

/// This system's file clock: round trips, a time between two seconds, and, where it is of the
/// Visual C++ shape, the times made from its count.
void this_systems_file_clock_converts() {
    using Clock = fs::file_time_type::clock;
    for (const int64_t known : {int64_t{0}, november_2026, july_1969, int64_t{-1}}) {
        OA_CHECK(file_seconds_since_1970(file_time_from_seconds(known)) == known);
        OA_CHECK(file_time_from_seconds(known) == clock_time_from_seconds<Clock>(known));
    }
    const fs::file_time_type::duration tick{1};
    OA_CHECK(file_seconds_since_1970(file_time_from_seconds(0) - tick) == -1);
    OA_CHECK(file_seconds_since_1970(file_time_from_seconds(july_1969) - tick) == july_1969 - 1);
    OA_CHECK(
        file_seconds_since_1970(file_time_from_seconds(november_2026) + tick) == november_2026
    );
    if constexpr (!ConvertsToSys<Clock>)
        ticks_from_1601_convert<Clock>();
}

/// The file clock's now is the system clock's, to the second or two between the readings.
void the_file_clocks_now_is_the_system_clocks() {
    const int64_t before = system_seconds();
    const int64_t now = file_seconds_since_1970(fs::file_time_type::clock::now());
    const int64_t after = system_seconds();
    OA_CHECK(now >= before - 2);
    OA_CHECK(now <= after + 2);
}

/// A file given a time keeps it, as a copy's record reads it back; a file just written reads as
/// now.
void a_file_keeps_the_time_it_is_given() {
    const fs::path folder = oa::test::make_scratch_directory("platform-file-time");
    const fs::path file = folder / "dated.bin";
    const int64_t before = system_seconds();
    {
        std::ofstream out(file, std::ios::binary);
        out << "bytes";
        OA_CHECK(static_cast<bool>(out));
    }
    const int64_t after = system_seconds();
    std::error_code error;
    const int64_t written = file_seconds_since_1970(fs::last_write_time(file, error));
    OA_CHECK(!error);
    OA_CHECK(written >= before - 2);
    OA_CHECK(written <= after + 2);
    fs::last_write_time(file, file_time_from_seconds(november_2026), error);
    OA_CHECK(!error);
    OA_CHECK(file_seconds_since_1970(fs::last_write_time(file, error)) == november_2026);
    OA_CHECK(!error);
    fs::remove_all(folder, error);
}

} // namespace

int main() {
    a_clock_without_to_sys_counts_from_1601();
    a_clock_with_to_sys_converts_through_it();
    this_systems_file_clock_converts();
    the_file_clocks_now_is_the_system_clocks();
    a_file_keeps_the_time_it_is_given();
    return oa::test::check_exit_status();
}
