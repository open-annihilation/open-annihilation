// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "bench.hpp"

#include "oa/platform/files.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <locale>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

namespace oa::app::bench {
namespace {

/// The most players a skirmish seats.
constexpr uint16_t kMostPlayers = 10;
/// The largest number of seconds or degrees a view may give.
constexpr double kLargestValue = 1.0e6;
/// Room for one printed line.
constexpr std::size_t kLineBytes = 160;

/// Returns the text with the blanks at both ends left off.
///
/// @param text the text
/// @return the text within it
[[nodiscard]] std::string_view trimmed(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r");
    if (first == std::string_view::npos)
        return {};
    const auto last = text.find_last_not_of(" \t\r");
    return text.substr(first, last - first + 1);
}

/// Reads a decimal number, of the whole text.
///
/// @param text the number
/// @param[out] value the number read
/// @return true when the whole text is a finite number within
///         ±kLargestValue
[[nodiscard]] bool read_number(std::string_view text, double& value) {
    // std::from_chars for double is missing from some of the standard
    // libraries the engine builds with, so the text is read by a stream in
    // the classic locale.
    std::istringstream stream{std::string(text)};
    stream.imbue(std::locale::classic());
    double read = 0;
    if (!(stream >> read) || !(stream >> std::ws).eof() || !std::isfinite(read) ||
        std::abs(read) > kLargestValue)
        return false;
    value = read;
    return true;
}

/// Reads a whole number of the whole text.
///
/// @param text the number
/// @param[out] value the number read
/// @return true when the whole text is a whole number from 0 to UINT32_MAX
[[nodiscard]] bool read_count(std::string_view text, uint32_t& value) {
    uint32_t read = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), read);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
        return false;
    value = read;
    return true;
}

} // namespace

bool parse_view(std::string_view text, View& view, std::string& error) {
    std::set<std::string, std::less<>> given;
    std::size_t line_number = 0;
    while (!text.empty()) {
        ++line_number;
        const auto end = text.find('\n');
        const std::string_view line =
            trimmed(end == std::string_view::npos ? text : text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        if (line.empty() || line.front() == '#')
            continue;
        const auto blank = line.find_first_of(" \t");
        const std::string_view key = line.substr(0, blank);
        const std::string_view value =
            blank == std::string_view::npos ? std::string_view{} : trimmed(line.substr(blank));
        const std::string where = "line " + std::to_string(line_number) + ": ";
        if (!given.emplace(key).second) {
            error = where + std::string(key) + " is given twice";
            return false;
        }
        if (value.empty()) {
            error = where + std::string(key) + " has no value";
            return false;
        }
        const auto seconds = [&](double& member) {
            double read = 0;
            if (!read_number(value, read) || read < 0) {
                error = where + std::string(key) + " takes seconds, 0 or more";
                return false;
            }
            member = read;
            return true;
        };
        const auto number = [&](double& member) {
            if (!read_number(value, member)) {
                error = where + std::string(key) + " takes a number";
                return false;
            }
            return true;
        };
        const auto ticks = [&](uint32_t& member) {
            if (!read_count(value, member)) {
                error = where + std::string(key) + " takes a whole number of ticks";
                return false;
            }
            return true;
        };
        bool read = true;
        if (key == "label") {
            view.label = std::string(value);
        } else if (key == "stage") {
            view.stage = oa::platform::path_from_utf8(std::string(value));
        } else if (key == "players") {
            uint32_t players = 0;
            if (!read_count(value, players) || players < 2 || players > kMostPlayers) {
                error = where + "players takes 2 to " + std::to_string(kMostPlayers);
                return false;
            }
            view.players = static_cast<uint16_t>(players);
        } else if (key == "menu_seconds") {
            read = seconds(view.menu_seconds);
        } else if (key == "warmup_ticks") {
            read = ticks(view.warmup_ticks);
        } else if (key == "measure_seconds") {
            read = seconds(view.measure_seconds);
        } else if (key == "measure_ticks") {
            read = ticks(view.measure_ticks);
        } else if (key == "shot_at") {
            read = seconds(view.shot_at);
        } else if (key == "shot_ticks") {
            read = ticks(view.shot_ticks);
        } else if (key == "centre_x") {
            read = number(view.centre_x);
        } else if (key == "centre_z") {
            read = number(view.centre_z);
        } else if (key == "yaw") {
            read = number(view.yaw);
        } else if (key == "pitch") {
            read = number(view.pitch);
        } else if (key == "zoom") {
            read = number(view.zoom);
            if (read && view.zoom < 0) {
                error = where + "zoom takes 0 for the farthest out, or a zoom above 0";
                return false;
            }
        } else if (key == "turn_rate") {
            read = number(view.turn_rate);
        } else if (key == "hold_after") {
            read = seconds(view.hold_after);
        } else if (key == "stage_timers" || key == "frame_log") {
            if (value != "0" && value != "1") {
                error = where + std::string(key) + " takes 0 or 1";
                return false;
            }
            (key == "stage_timers" ? view.stage_timers : view.frame_log) = value == "1";
        } else {
            error = where + "the bench has no key " + std::string(key);
            return false;
        }
        if (!read)
            return false;
    }
    return true;
}

bool read_view(const std::filesystem::path& file, View& view, std::string& error) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        error = "cannot read the bench file " + oa::platform::path_to_utf8(file);
        return false;
    }
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    View read{};
    read.label = oa::platform::path_to_utf8(file.stem());
    if (!parse_view(text, read, error)) {
        error = oa::platform::path_to_utf8(file) + ": " + error;
        return false;
    }
    if (!read.stage.empty() && read.stage.is_relative())
        read.stage = file.parent_path() / read.stage;
    view = std::move(read);
    return true;
}

Spread spread(std::span<const double> values) {
    if (values.empty())
        return {};
    std::vector<double> sorted(values.begin(), values.end());
    std::sort(sorted.begin(), sorted.end());
    const std::size_t count = sorted.size();
    const auto percentile = [&](std::size_t percent) {
        return sorted[std::min(count - 1, count * percent / 100)];
    };
    double total = 0;
    for (const double value : sorted)
        total += value;
    const std::size_t slowest = (count + 99) / 100;
    double slowest_total = 0;
    for (std::size_t index = count - slowest; index < count; ++index)
        slowest_total += sorted[index];
    Spread result{};
    result.least = sorted.front();
    result.mean = total / static_cast<double>(count);
    result.p50 = percentile(50);
    result.p95 = percentile(95);
    result.p99 = percentile(99);
    result.low1 = slowest_total / static_cast<double>(slowest);
    result.most = sorted.back();
    return result;
}

std::vector<std::size_t> slowest_frames(std::span<const double> values) {
    std::vector<std::size_t> frames(values.size());
    for (std::size_t index = 0; index < frames.size(); ++index)
        frames[index] = index;
    std::sort(frames.begin(), frames.end(), [&](std::size_t left, std::size_t right) {
        return values[left] != values[right] ? values[left] < values[right] : left < right;
    });
    const std::size_t slowest = (frames.size() + 99) / 100;
    frames.erase(frames.begin(), frames.end() - static_cast<std::ptrdiff_t>(slowest));
    std::sort(frames.begin(), frames.end());
    return frames;
}

double frames_per_second(double milliseconds) {
    return milliseconds > 0 ? 1000.0 / milliseconds : 0.0;
}

std::string measure_line(std::string_view name, const Spread& values) {
    char line[kLineBytes];
    std::snprintf(
        line,
        sizeof line,
        "bench: MEASURE %-16.*s min %8.2f avg %8.2f p50 %8.2f p95 %8.2f p99 %8.2f low1 %8.2f max "
        "%8.2f",
        static_cast<int>(name.size()),
        name.data(),
        values.least,
        values.mean,
        values.p50,
        values.p95,
        values.p99,
        values.low1,
        values.most
    );
    return line;
}

namespace {

/// Returns a line of a stage's mean milliseconds a frame, its name
/// indented two spaces when it is part of the stage before it.
///
/// @param kind the line's kind after "bench: ", such as STAGE
/// @param stage the stage
/// @param milliseconds its time over the frames
/// @param frames how many they are
/// @return the line, with no newline
[[nodiscard]] std::string stage_mean_line(
    const char* kind, bench_timers::Stage stage, double milliseconds, std::size_t frames
) {
    const auto& name = bench_timers::stage_names[static_cast<std::size_t>(stage)];
    const std::string shown = (name.nested ? "  " : "") + std::string(name.name);
    char line[kLineBytes];
    std::snprintf(
        line,
        sizeof line,
        "bench: %s %-36s %8.2f",
        kind,
        shown.c_str(),
        frames == 0 ? 0.0 : milliseconds / static_cast<double>(frames)
    );
    return line;
}

} // namespace

std::string stage_line(bench_timers::Stage stage, double milliseconds, std::size_t frames) {
    return stage_mean_line("STAGE", stage, milliseconds, frames);
}

std::string slow_stage_line(bench_timers::Stage stage, double milliseconds, std::size_t frames) {
    return stage_mean_line("SLOW STAGE", stage, milliseconds, frames);
}

} // namespace oa::app::bench
