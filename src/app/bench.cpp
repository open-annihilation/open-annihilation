// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "bench.hpp"

#include "oa/data/enhanced_mode.hpp"
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
/// Room for a SIM_FRAME line with full-width tick numbers and times.
constexpr std::size_t simulation_line_bytes = 256;
/// The frame receiving existing step timings on this thread, only inside a bench scope.
thread_local SimulationFrame* simulation_frame{};

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

/// Joins every known feature's name.
///
/// @return the names, in id order, separated by commas
[[nodiscard]] std::string known_feature_names() {
    std::string names;
    for (const oa::data::enhanced_mode::FeatureInfo& row :
         oa::data::enhanced_mode::feature_table()) {
        if (!names.empty())
            names += ", ";
        names.append(row.name);
    }
    return names;
}

} // namespace

bool resolve_feature_list(
    std::string_view text,
    uint32_t revision,
    oa::data::enhanced_mode::FeatureSet& features,
    std::string& error,
    std::string_view subject
) {
    namespace mode = oa::data::enhanced_mode;
    const std::string subject_text(subject);
    std::vector<std::string_view> items;
    std::size_t at = 0;
    while (at <= text.size()) {
        const auto comma = text.find(',', at);
        const std::string_view item = trimmed(
            comma == std::string_view::npos ? text.substr(at) : text.substr(at, comma - at)
        );
        if (item.empty()) {
            error = subject_text + " needs a feature name";
            return false;
        }
        items.push_back(item);
        if (comma == std::string_view::npos)
            break;
        at = comma + 1;
    }
    bool signed_list = false;
    bool bare = false;
    for (std::string_view item : items) {
        if (item.front() == '+' || item.front() == '-') {
            signed_list = true;
            item.remove_prefix(1);
            item = trimmed(item);
        } else {
            bare = true;
        }
        if (item.empty()) {
            error = subject_text + " needs a feature name";
            return false;
        }
        if (!mode::feature_by_name(item)) {
            error = subject_text + ": unknown feature " + std::string(item) +
                    "; known features: " + known_feature_names();
            return false;
        }
    }
    if (signed_list && bare) {
        error = subject_text + " is a list of names, or every item starts with + or -";
        return false;
    }
    if (!signed_list) {
        mode::FeatureSet set;
        for (const std::string_view item : items)
            set = set.with(*mode::feature_by_name(item));
        if (const char* problem = mode::feature_set_problem(set)) {
            error = problem;
            return false;
        }
        features = set;
        return true;
    }
    if (revision < 1 || revision > mode::highest_revision ||
        mode::revision_features(revision).empty()) {
        error = subject_text + " needs a revision this build plays";
        return false;
    }
    mode::FeatureSet set = mode::revision_features(revision);
    mode::FeatureSet removed;
    for (std::string_view item : items) {
        const bool add = item.front() == '+';
        item.remove_prefix(1);
        const mode::Feature feature = *mode::feature_by_name(trimmed(item));
        if (add) {
            set = mode::close_features(set.with(feature));
            removed = removed.without(feature);
        } else {
            set = set.without(feature);
            removed = removed.with(feature);
        }
    }
    const mode::FeatureSet closed = mode::close_features(set);
    for (const mode::FeatureInfo& row : mode::feature_table()) {
        if (!removed.has(row.id) || !closed.has(row.id))
            continue;
        std::string_view needer;
        for (const mode::FeatureInfo& other : mode::feature_table()) {
            if (closed.has(other.id) && other.needs.has(row.id)) {
                needer = other.name;
                break;
            }
        }
        error = subject_text + " cannot remove " + std::string(row.name) + ": " +
                std::string(needer.empty() ? std::string_view{"a feature"} : needer) + " needs it";
        return false;
    }
    if (const char* problem = mode::feature_set_problem(closed)) {
        error = problem;
        return false;
    }
    features = closed;
    return true;
}

std::string feature_list_text(oa::data::enhanced_mode::FeatureSet features) {
    namespace mode = oa::data::enhanced_mode;
    if (features.empty())
        return "-";
    std::string names;
    for (const mode::FeatureInfo& row : mode::feature_table()) {
        if (!features.has(row.id))
            continue;
        if (!names.empty())
            names += ',';
        names.append(row.name);
    }
    return names.empty() ? std::string("-") : names;
}

SimulationFrameScope::SimulationFrameScope(SimulationFrame* frame) noexcept
    : frame_(frame), previous_(simulation_frame) {
    simulation_frame = frame;
}

SimulationFrameScope::~SimulationFrameScope() {
    if (frame_ && previous_ && frame_ != previous_ && frame_->steps != 0) {
        if (previous_->steps == 0 || frame_->slowest_ns > previous_->slowest_ns) {
            previous_->slowest_ns = frame_->slowest_ns;
            previous_->slowest_tick = frame_->slowest_tick;
        }
        previous_->steps += frame_->steps;
    }
    simulation_frame = previous_;
}

void note_simulation_step(uint32_t tick, uint64_t elapsed_ns) noexcept {
    if (!simulation_frame)
        return;
    if (simulation_frame->steps == 0 || elapsed_ns > simulation_frame->slowest_ns) {
        simulation_frame->slowest_ns = elapsed_ns;
        simulation_frame->slowest_tick = tick;
    }
    ++simulation_frame->steps;
}

std::string
simulation_frame_line(std::size_t index, double milliseconds, const SimulationFrame& frame) {
    constexpr double nanoseconds_per_millisecond = 1.0e6;
    char line[simulation_line_bytes];
    std::snprintf(
        line,
        sizeof line,
        "bench: SIM_FRAME %zu sim_ms %.3f tick_before %u tick_after %u ticks %u steps %u "
        "max_step_ms %.3f max_step_tick %u",
        index,
        milliseconds,
        frame.tick_before,
        frame.tick_after,
        uint32_t(frame.tick_after - frame.tick_before),
        frame.steps,
        static_cast<double>(frame.slowest_ns) / nanoseconds_per_millisecond,
        frame.slowest_tick
    );
    return line;
}

bool parse_view(std::string_view text, View& view, std::string& error) {
    std::set<std::string, std::less<>> given;
    std::size_t line_number = 0;
    std::size_t features_line = 0;
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
        const auto frames = [&](uint32_t& member) {
            if (!read_count(value, member)) {
                error = where + std::string(key) + " takes a whole number of frames";
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
        } else if (key == "enhanced_revision") {
            uint32_t revision = 0;
            if (!read_count(value, revision) || revision < 1 ||
                revision > oa::data::enhanced_mode::highest_revision) {
                error = where + "enhanced_revision takes a revision this build plays";
                return false;
            }
            view.enhanced_revision = revision;
        } else if (key == "enhanced_features") {
            view.enhanced_features = std::string(value);
            view.enhanced_features_named = true;
            features_line = line_number;
        } else if (key == "menu_seconds") {
            read = seconds(view.menu_seconds);
        } else if (key == "warmup_ticks") {
            read = ticks(view.warmup_ticks);
        } else if (key == "warmup_frames") {
            read = frames(view.warmup_frames);
        } else if (key == "measure_seconds") {
            read = seconds(view.measure_seconds);
        } else if (key == "measure_ticks") {
            read = ticks(view.measure_ticks);
        } else if (key == "measure_frames") {
            read = frames(view.measure_frames);
        } else if (key == "expected_live_units") {
            if (!read_count(value, view.expected_live_units)) {
                error = where + "expected_live_units takes a whole number of units";
                return false;
            }
        } else if (key == "expected_initial_units") {
            read = frames(view.expected_initial_units);
        } else if (key == "expected_initial_per_side") {
            read = frames(view.expected_initial_per_side);
        } else if (key == "expected_output_width") {
            read = frames(view.expected_output_width);
        } else if (key == "expected_output_height") {
            read = frames(view.expected_output_height);
        } else if (key == "display_number") {
            read = frames(view.display_number);
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
        } else if (
            key == "stage_timers" || key == "frame_log" || key == "plan_work" ||
            key == "require_paused" || key == "require_gameplay"
        ) {
            if (value != "0" && value != "1") {
                error = where + std::string(key) + " takes 0 or 1";
                return false;
            }
            (key == "stage_timers"       ? view.stage_timers
             : key == "frame_log"        ? view.frame_log
             : key == "plan_work"        ? view.plan_work
             : key == "require_gameplay" ? view.require_gameplay
                                         : view.require_paused) = value == "1";
        } else {
            error = where + "the bench has no key " + std::string(key);
            return false;
        }
        if (!read)
            return false;
    }
    if (view.enhanced_features_named) {
        namespace mode = oa::data::enhanced_mode;
        const uint32_t revision =
            view.enhanced_revision != 0 ? view.enhanced_revision : mode::highest_revision;
        mode::FeatureSet resolved;
        if (!resolve_feature_list(
                view.enhanced_features, revision, resolved, error, "enhanced_features"
            )) {
            error = "line " + std::to_string(features_line) + ": " + error;
            return false;
        }
    }
    if (view.require_paused &&
        (view.warmup_ticks != 0 || view.measure_ticks != 0 || view.turn_rate != 0)) {
        error = "require_paused needs warmup_ticks, measure_ticks and turn_rate set to 0";
        return false;
    }
    if (view.require_gameplay && (view.require_paused || view.expected_live_units != 0 ||
                                  view.measure_ticks == 0 || view.turn_rate != 0)) {
        error =
            "require_gameplay needs an unpaused tick span, fixed camera and initial-only counts";
        return false;
    }
    if ((view.expected_output_width == 0) != (view.expected_output_height == 0)) {
        error = "expected_output_width and expected_output_height must both be given";
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

bool validate_rules_request(const View& view, bool enhanced, std::string& error) {
    if (view.enhanced_revision == 0 && !view.enhanced_features_named)
        return true;
    if (!enhanced) {
        error = view.enhanced_revision != 0 ? "enhanced_revision requires Enhanced Mode"
                                            : "enhanced_features requires Enhanced Mode";
        return false;
    }
    return true;
}

bool validate_started_rules(
    const View& view, const oa::data::enhanced_mode::Choice& actual, std::string& error
) {
    namespace mode = oa::data::enhanced_mode;
    if (const char* problem = mode::choice_problem(actual)) {
        error = problem;
        return false;
    }
    if (view.enhanced_revision != 0 && actual.revision != view.enhanced_revision) {
        error = "the skirmish's Enhanced Mode revision differs from enhanced_revision";
        return false;
    }
    if (!view.enhanced_features_named)
        return true;
    const uint32_t revision =
        view.enhanced_revision != 0 ? view.enhanced_revision : mode::highest_revision;
    mode::FeatureSet asked;
    if (!resolve_feature_list(view.enhanced_features, revision, asked, error, "enhanced_features"))
        return false;
    if (actual.features != asked) {
        error = "the skirmish's Enhanced Mode features differ from enhanced_features";
        return false;
    }
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
