// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Day numbers of civil dates, and the runs of a list that share a day.

#include "oa/data/replays/replays.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace oa::data::replays {
namespace {

/// Days in a 400-year cycle of the Gregorian calendar.
constexpr int64_t days_per_era = 146097;

/// Days from 0000-03-01, where the count of eras starts, to 1970-01-01.
constexpr int64_t days_before_1970 = 719468;

} // namespace

int32_t day_number(int32_t year, uint32_t month, uint32_t day) noexcept {
    // Years are counted from March, so that the leap day ends a year.
    const int64_t march_year = static_cast<int64_t>(year) - (month <= 2U ? 1 : 0);
    const int64_t era = (march_year >= 0 ? march_year : march_year - 399) / 400;
    const int64_t year_of_era = march_year - era * 400;
    const int64_t month_from_march =
        month > 2U ? static_cast<int64_t>(month) - 3 : static_cast<int64_t>(month) + 9;
    const int64_t day_of_year = (153 * month_from_march + 2) / 5 + static_cast<int64_t>(day) - 1;
    const int64_t day_of_era =
        year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    const int64_t days = era * days_per_era + day_of_era - days_before_1970;
    if (days < std::numeric_limits<int32_t>::min())
        return std::numeric_limits<int32_t>::min();
    if (days > std::numeric_limits<int32_t>::max())
        return std::numeric_limits<int32_t>::max();
    return static_cast<int32_t>(days);
}

std::vector<DayGroup> group_by_day(std::span<const int32_t> days) {
    std::vector<DayGroup> groups;
    for (std::size_t at = 0; at < days.size(); ++at) {
        if (!groups.empty() && groups.back().day == days[at]) {
            ++groups.back().count;
            continue;
        }
        DayGroup group;
        group.day = days[at];
        group.first = at;
        group.count = 1;
        groups.push_back(group);
    }
    return groups;
}

} // namespace oa::data::replays
