// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The file name of a photo of the battlefield. Pure: no window, no drawing
// and no folders. The runtime supplies the clock, the map and how long a
// path may be.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace oa::present::photo {

/// The local date and time a photo is named with.
struct LocalMoment {
    int year = 0;   ///< the full year, such as 2026
    int month = 1;  ///< the month, 1 to 12
    int day = 1;    ///< the day of the month, 1 to 31
    int hour = 0;   ///< the hour, 0 to 23
    int minute = 0; ///< the minute, 0 to 59
    int second = 0; ///< the second, 0 to 60
};

/// Counts a text the way a path counts it: bytes, or UTF-16 units on Windows.
///
/// A well-formed UTF-8 sequence counts as one character on Windows when it
/// fits in one UTF-16 unit, and as two when it does not. A byte that does
/// not begin a well-formed sequence counts as one.
///
/// @param text the text
/// @return the count
[[nodiscard]] std::size_t path_characters(std::string_view text) noexcept;

/// Builds a photo's file name: `Photo <YYYY-MM-DD> <HH.MM.SS> <map>.png`.
///
/// Characters a file system refuses in the map (`<>:"/\\|?*`, control
/// characters, and a trailing run of dots and spaces) become a space. The
/// map is shortened by whole UTF-8 characters, from its end, until the path
/// fits `path_budget` characters counting `folder_characters` plus the
/// separator. The date, the time, a collision's ` 2` and `.png` stay. The
/// first photo of a second has no number. A `copy` of 2 or more is written
/// before `.png`, so the second is ` 2` and the third is ` 3`. An empty map
/// leaves no space before `.png`.
///
/// @param moment the local date and time
/// @param map_name the map's name; empty leaves the name without a map
/// @param copy 1 for the first photo of that second, then 2, 3 and on
/// @param folder_characters the folder's length in path characters
/// @param path_budget the most path characters the whole path may have
/// @return the file name, without a folder
[[nodiscard]] std::string photo_file_name(
    const LocalMoment& moment,
    std::string_view map_name,
    int copy,
    std::size_t folder_characters,
    std::size_t path_budget
);

} // namespace oa::present::photo
