// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// How long a recorded replay is kept, and which replays the rule never takes.

#include "oa/data/replays/replays.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace oa::data::replays {
namespace {

/// Tells whether a path is a file inside a folder of Recordings.
///
/// @param path the replay's path, '/' between folders
/// @param folder the folder's name
/// @return true when the path starts with that folder and a slash
bool in_folder(std::string_view path, std::string_view folder) noexcept {
    return path.size() > folder.size() && path.substr(0, folder.size()) == folder &&
           path[folder.size()] == '/';
}

} // namespace

KeepSpan keep_span_of(std::string_view text) noexcept {
    if (text == keep_90_days)
        return KeepSpan::days_90;
    if (text == keep_one_year)
        return KeepSpan::one_year;
    if (text == keep_forever)
        return KeepSpan::forever;
    return KeepSpan::days_30;
}

std::string_view keep_span_text(KeepSpan span) noexcept {
    switch (span) {
    case KeepSpan::days_30:
        return keep_30_days;
    case KeepSpan::days_90:
        return keep_90_days;
    case KeepSpan::one_year:
        return keep_one_year;
    case KeepSpan::forever:
        return keep_forever;
    }
    return keep_30_days;
}

std::optional<int32_t> keep_span_days(KeepSpan span) noexcept {
    switch (span) {
    case KeepSpan::days_30:
        return keep_span_30;
    case KeepSpan::days_90:
        return keep_span_90;
    case KeepSpan::one_year:
        return keep_span_year;
    case KeepSpan::forever:
        return std::nullopt;
    }
    return keep_span_30;
}

bool downloaded(const Replay& replay) noexcept {
    return in_folder(replay.path, core_prime_folder_name) || replay.how == how_core_prime;
}

bool exported(const Replay& replay) noexcept {
    return replay.exported;
}

bool retention_spares(const Replay& replay) noexcept {
    return replay.kept || imported(replay) || replay.how == how_imported || downloaded(replay) ||
           exported(replay);
}

bool spared_by_set_aside(const Replay& replay, std::optional<int64_t> set_aside_at) noexcept {
    // A mark cut off with a damaged kept.json cannot be known, and only a
    // replay that was there when the file was set aside can have had one.
    return set_aside_at.has_value() && replay.recorded <= *set_aside_at;
}

bool due_to_go(const Replay& replay, int32_t recorded_day, KeepSpan span, int32_t today) noexcept {
    if (retention_spares(replay))
        return false;
    const std::optional<int32_t> days = keep_span_days(span);
    if (!days)
        return false;
    // The day it was recorded counts as the first day. It goes the day after
    // the span has passed, so an age of exactly the span stays.
    const auto last = static_cast<int64_t>(recorded_day) + static_cast<int64_t>(*days);
    return static_cast<int64_t>(today) > last;
}

} // namespace oa::data::replays
