// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/app/frame_pacing.hpp"

#include "oa/ui/hud/camera_scroll.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string_view>

namespace oa::app::frame_pacing {

using oa::base::game_loop::normal_game_speed;

namespace {

/// Milliseconds in a second, the divisor scaled_clock takes the clock units by.
constexpr uint32_t kMillisecondsPerSecond = 1000;
/// A clock unit and a half, in thousandths of a unit: scaled_clock's
/// product counts kMillisecondsPerSecond to a unit.
constexpr uint32_t kUnitAndHalfThousandths = kMillisecondsPerSecond * 3 / 2;
/// Runs of paced frames longer than this begin again from their last frame,
/// so the frame count times a second stays far inside 64 bits.
constexpr uint64_t kFramesPerRunLimit = 1'000'000;
/// Game.current_speed is tenths of normal speed.
constexpr double kSpeedTenths = 0.1;
/// Nanoseconds in a millisecond, as a double for the overlay's figures.
constexpr double kNanosecondsPerMillisecondReal = 1.0e6;
/// Text a measure shows before the first whole second.
constexpr std::string_view kNoMeasure = "--";

/// Returns the spread of one measure.
///
/// @param spreads a second's spreads
/// @param measure the measure
/// @return its spread
const MeasureSpread& spread_of(
    const std::array<MeasureSpread, kFrameMeasureCount>& spreads, FrameMeasure measure
) noexcept {
    return spreads[static_cast<std::size_t>(measure)];
}

/// Formats a count over a span as a whole number a second.
///
/// @param count samples in the span
/// @param span_ns the span, nanoseconds, above 0
/// @return the rate, rounded to the nearest whole number
uint64_t per_second(uint32_t count, uint64_t span_ns) noexcept {
    return (static_cast<uint64_t>(count) * kNanosecondsPerSecond + span_ns / 2) / span_ns;
}

/// Writes a text into one of the table's texts, cut short to fit.
///
/// @param[out] out the table's text; its grade is left as it was
/// @param text the text
void set_text(FrameStatsText& out, std::string_view text) noexcept {
    const std::size_t length = std::min(text.size(), out.text.size() - 1);
    std::copy_n(text.data(), length, out.text.data());
    out.text[length] = '\0';
}

/// Writes a whole number into one of the table's texts.
///
/// @param[out] out the table's text; its grade is left as it was
/// @param value the number
void set_number(FrameStatsText& out, uint64_t value) noexcept {
    std::snprintf(out.text.data(), out.text.size(), "%llu", static_cast<unsigned long long>(value));
}

/// Writes a time in milliseconds to two decimals into one of the table's
/// texts, with its grade.
///
/// @param[out] out the table's text
/// @param elapsed_ns the time, nanoseconds
/// @param severity its grade
void set_time(FrameStatsText& out, uint64_t elapsed_ns, TimeSeverity severity) noexcept {
    std::snprintf(
        out.text.data(),
        out.text.size(),
        "%.2f",
        static_cast<double>(elapsed_ns) / kNanosecondsPerMillisecondReal
    );
    out.severity = severity;
}

/// Fills a measure's row: its least, mean and most times.
///
/// @param[out] row the row, its kind and label already set
/// @param spread its samples over the shown second
/// @param shown a whole second has been measured
void set_measure_row(FrameStatsRow& row, const MeasureSpread& spread, bool shown) noexcept {
    if (!shown || spread.count == 0) {
        set_text(row.values[kFrameStatsMeanColumn], kNoMeasure);
        return;
    }
    set_time(row.values[kFrameStatsLeastColumn], spread.least_ns, spread.least_severity);
    const uint64_t mean = spread.total_ns / spread.count;
    set_time(
        row.values[kFrameStatsMeanColumn],
        mean,
        time_severity(mean, spread.allowance_total_ns / spread.count)
    );
    set_time(row.values[kFrameStatsMostColumn], spread.most_ns, spread.most_severity);
}

/// The "+stats" table's row of the title.
constexpr std::size_t kTitleRow = 0;
/// The row of the value columns' names.
constexpr std::size_t kHeadingRow = 1;
/// The row of the frames a second.
constexpr std::size_t kRateRow = 2;
/// The first measure's row; the others follow in FrameMeasure's order.
constexpr std::size_t kFirstMeasureRow = 3;
/// The row of the units drawn.
constexpr std::size_t kCountRow = kFirstMeasureRow + kFrameMeasureCount;
/// The row of the renderer.
constexpr std::size_t kRendererRow = kCountRow + 1;
/// The row of the window and the display.
constexpr std::size_t kDisplayRow = kRendererRow + 1;
static_assert(kDisplayRow + 1 == kFrameStatsRowsMost);
/// The top two bits of a UTF-8 byte, which tell a continuation byte.
constexpr unsigned kUtf8LeadMask = 0xC0;
/// The top two bits of a UTF-8 continuation byte.
constexpr unsigned kUtf8Continuation = 0x80;
/// What joins the tier and the render driver in the renderer row's label.
constexpr std::string_view kTierDriverJoin = ": ";
/// What joins the tier and its anti-aliasing in the renderer row's label.
constexpr std::string_view kTierAntiAliasingJoin = " ";
/// The characters one of the table's texts holds, its terminating zero
/// left out.
constexpr std::size_t kTextCharacters = kFrameStatsTextBytes - 1;
/// The most characters a whole number of a type is written in, its sign
/// included.
template <typename Number>
constexpr std::size_t kNumberCharacters = std::numeric_limits<Number>::digits10 + 2;
/// Bytes the display row's mode takes at its widest, two whole numbers and a
/// rate joined by "x" and "@", its terminating zero included.
constexpr std::size_t kModeTextBytes = 2 * kNumberCharacters<int> + kNumberCharacters<long> + 3;
/// Bytes the display row's note takes at its widest before it is cut to
/// fit: the mode, a space and the scale, its terminating zero included.
constexpr std::size_t kNoteTextBytes = kModeTextBytes + kFrameStatsTextBytes;

/// Starts the "+stats" table: every row's kind and label, the title and the
/// value columns' names.
///
/// @return the table with its values and notes empty
FrameStatsTable table_rows() noexcept {
    FrameStatsTable table{};
    table.row_count = kFrameStatsRowsMost;
    auto& title = table.rows[kTitleRow];
    title.kind = FrameStatsRowKind::title;
    set_text(title.label, "Frame stats (ms)");
    auto& heading = table.rows[kHeadingRow];
    heading.kind = FrameStatsRowKind::heading;
    set_text(heading.values[kFrameStatsLeastColumn], "min");
    set_text(heading.values[kFrameStatsMeanColumn], "avg");
    set_text(heading.values[kFrameStatsMostColumn], "max");
    table.rows[kRateRow].kind = FrameStatsRowKind::rate;
    set_text(table.rows[kRateRow].label, "FPS");
    constexpr std::array<std::string_view, kFrameMeasureCount> kNames{
        "frame", "work", "tick", "draw", "present"
    };
    for (std::size_t measure = 0; measure < kFrameMeasureCount; ++measure) {
        auto& row = table.rows[kFirstMeasureRow + measure];
        row.kind = FrameStatsRowKind::measure;
        set_text(row.label, kNames[measure]);
    }
    table.rows[kCountRow].kind = FrameStatsRowKind::count;
    set_text(table.rows[kCountRow].label, "units");
    table.rows[kRendererRow].kind = FrameStatsRowKind::renderer;
    table.rows[kDisplayRow].kind = FrameStatsRowKind::display;
    return table;
}

/// Fills the renderer row: the tier and the driver as its label, and the
/// adapter, or a limit the tier draws within, as its note, each cut to one
/// of the table's texts, never inside a character.
///
/// @param[out] row the row, its kind already set
/// @param renderer the renderer to name
void set_renderer_row(FrameStatsRow& row, const FrameStatsRenderer& renderer) noexcept {
    // A byte more than a text holds, so that the cut can see whether the
    // character at the limit goes on past it.
    std::array<char, kTextCharacters + 1> label{};
    std::size_t length = 0;
    const auto append = [&](std::string_view part) {
        const std::size_t taken = std::min(part.size(), label.size() - length);
        std::copy_n(part.data(), taken, label.data() + length);
        length += taken;
    };
    append(renderer.tier);
    if (!renderer.anti_aliasing.empty()) {
        append(kTierAntiAliasingJoin);
        append(renderer.anti_aliasing);
    }
    if (!renderer.driver.empty()) {
        append(kTierDriverJoin);
        append(renderer.driver);
    }
    set_text(row.label, whole_characters({label.data(), length}, kTextCharacters));
    set_text(
        row.note,
        whole_characters(
            renderer.limit.empty() ? renderer.adapter : renderer.limit, kTextCharacters
        )
    );
}

/// Returns a scale as the display row writes it: at most two decimals,
/// without trailing zeros or a trailing point, and an "x", as "2x" or
/// "1.25x".
///
/// @param scale the scale, above 0
/// @param[out] out where the text goes
/// @param bytes the bytes `out` holds, its terminating zero included
void write_scale(float scale, char* out, std::size_t bytes) noexcept {
    std::array<char, kFrameStatsTextBytes> digits{};
    std::snprintf(digits.data(), digits.size(), "%.2f", static_cast<double>(scale));
    std::string_view text{digits.data()};
    if (const auto point = text.find('.'); point != std::string_view::npos) {
        while (text.size() > point + 1 && text.back() == '0')
            text.remove_suffix(1);
        if (text.size() == point + 1)
            text.remove_suffix(1);
    }
    std::snprintf(out, bytes, "%.*sx", static_cast<int>(text.size()), text.data());
}

/// Returns the frame graph's column a time lies in.
///
/// @param time_ns nanoseconds from the first frame's start
/// @return the column, numbered from 0
uint64_t graph_column(uint64_t time_ns) noexcept {
    return time_ns / kNanosecondsPerSecond * kFrameGraphColumnsPerSecond +
           time_ns % kNanosecondsPerSecond * kFrameGraphColumnsPerSecond / kNanosecondsPerSecond;
}

/// Counts the frame graph's columns whose middle lies at or before a time.
///
/// @param time_ns nanoseconds from the first frame's start
/// @return the columns, from column 0
uint64_t middles_by(uint64_t time_ns) noexcept {
    // Column c's middle lies (2c + 1) / (2 * kFrameGraphColumnsPerSecond)
    // seconds in.
    constexpr uint64_t kHalfColumnsPerSecond = 2 * kFrameGraphColumnsPerSecond;
    return time_ns / kNanosecondsPerSecond * kFrameGraphColumnsPerSecond +
           (time_ns % kNanosecondsPerSecond * kHalfColumnsPerSecond + kNanosecondsPerSecond) /
               (2 * kNanosecondsPerSecond);
}

} // namespace

uint64_t
paced_frame_due(uint64_t run_start_ns, uint64_t frame, uint32_t frames_per_second) noexcept {
    return run_start_ns + frame * kNanosecondsPerSecond / frames_per_second;
}

uint64_t next_clock_unit_middle(uint64_t time_ns) noexcept {
    const uint64_t milliseconds = time_ns / kNanosecondsPerMillisecond;
    // How far into its unit the clock's millisecond lies, in thousandths of
    // a unit: the remainder of scaled_clock's product, which wraps at 32 bits.
    const uint32_t into =
        static_cast<uint32_t>(milliseconds) * kTicksPerSecond % kMillisecondsPerSecond;
    // The next unit's middle lies a unit and a half, less that far, after
    // the millisecond; a thousandth of a unit is 1 / kTicksPerSecond ms.
    const uint64_t to_middle = kUnitAndHalfThousandths - into;
    return milliseconds * kNanosecondsPerMillisecond +
           to_middle * kNanosecondsPerMillisecond / kTicksPerSecond;
}

uint64_t begin_paced_frame(FramePacer& pacer, uint64_t now_ns) noexcept {
    pacer.frame_start_ns = now_ns;
    const bool paced = pacer.started && pacer.frames_per_second != 0;
    if (paced) {
        const uint64_t due =
            paced_frame_due(pacer.run_start_ns, pacer.frames_in_run, pacer.frames_per_second);
        const uint64_t period = kNanosecondsPerSecond / pacer.frames_per_second;
        if (now_ns >= due && now_ns - due < period) {
            pacer.frame_start_ns = due;
            return due;
        }
    }
    pacer.run_start_ns = now_ns;
    pacer.frames_in_run = 0;
    pacer.started = true;
    return now_ns;
}

uint64_t end_paced_frame(FramePacer& pacer, uint64_t now_ns, uint32_t frames_per_second) noexcept {
    if (frames_per_second == 0) {
        pacer.frames_per_second = 0;
        return 0;
    }
    if (frames_per_second != pacer.frames_per_second || !pacer.started) {
        pacer.frames_per_second = frames_per_second;
        pacer.run_start_ns = pacer.started ? pacer.frame_start_ns : now_ns;
        pacer.frames_in_run = 0;
    }
    uint64_t due = 0;
    if (frames_per_second == kTicksPerSecond) {
        // One frame a clock unit: the next is due at the middle of the unit
        // after the frame's, as the first frame of a run of its own.
        due = next_clock_unit_middle(pacer.started ? pacer.frame_start_ns : now_ns);
        pacer.run_start_ns = due;
        pacer.frames_in_run = 0;
    } else {
        if (pacer.frames_in_run >= kFramesPerRunLimit) {
            pacer.run_start_ns =
                paced_frame_due(pacer.run_start_ns, pacer.frames_in_run, frames_per_second);
            pacer.frames_in_run = 0;
        }
        ++pacer.frames_in_run;
        due = paced_frame_due(pacer.run_start_ns, pacer.frames_in_run, frames_per_second);
    }
    if (due <= now_ns) {
        pacer.run_start_ns = now_ns;
        pacer.frames_in_run = 0;
        return 0;
    }
    return due - now_ns;
}

uint32_t paced_frame_rate(uint32_t max_frames_per_second, const FrameActivity& activity) noexcept {
    if (activity.match_advancing || activity.camera_moving || activity.input_recent ||
        activity.unattended)
        return max_frames_per_second;
    if (max_frames_per_second == 0)
        return kIdleFramesPerSecond;
    return std::min(max_frames_per_second, kIdleFramesPerSecond);
}

uint32_t vsync_frame_cap(float display_hz) noexcept {
    // Not a number, 0 and below: the display reports no rate.
    if (!(display_hz > 0.0F))
        return 0;
    // A whole number of hertz has the next number down below it; any other
    // rate the whole number at or under it.
    const float bounded = std::min(display_hz, static_cast<float>(kHighestVsyncCap + 1));
    const auto below = static_cast<uint32_t>(std::ceil(bounded)) - 1U;
    return std::max(below, kTicksPerSecond);
}

uint32_t capped_frame_rate(uint32_t frames_per_second, uint32_t cap) noexcept {
    if (cap == 0)
        return frames_per_second;
    if (frames_per_second == 0)
        return cap;
    return std::min(frames_per_second, cap);
}

float next_presentation_alpha(
    TickPresentation& presentation,
    const oa::base::game_loop::Timing& timing,
    const FrameTicks& ticks,
    uint64_t now_ns
) noexcept {
    if (ticks.whole) {
        presentation = {};
        return presentation.alpha;
    }
    // The most ticks one batch runs at the clock's speed: one a unit at
    // normal speed and below, the speed's whole multiple above it.
    const uint32_t batch_limit = std::max<uint32_t>(
        1, (static_cast<uint32_t>(timing.actual_rate) + normal_game_speed - 1) / normal_game_speed
    );
    const bool batch = ticks.ran != 0;
    if (batch)
        presentation.hold =
            ticks.ran < static_cast<uint32_t>(std::max(ticks.owed, 0)) || ticks.ran > batch_limit;
    else if (ticks.owed > 0)
        presentation.hold = true;
    if (presentation.hold) {
        presentation.alpha = 1.0F;
        return presentation.alpha;
    }
    // The clock units since the clock last stepped and the fraction of the
    // current one, read as scaled_clock reads the milliseconds (the product
    // wraps at 32 bits), with the time within the millisecond added.
    const auto now_ms = static_cast<uint32_t>(now_ns / kNanosecondsPerMillisecond);
    const uint32_t scaled = now_ms * kTicksPerSecond;
    const uint32_t whole_units = scaled / kMillisecondsPerSecond;
    const double within_ms = static_cast<double>(now_ns % kNanosecondsPerMillisecond) /
                             static_cast<double>(kNanosecondsPerMillisecond);
    // Drawn one a clock unit, a frame counts the current unit whole: the
    // next frame is due at the next unit.
    const double unit_fraction = ticks.unit_frames
                                     ? 1.0
                                     : (static_cast<double>(scaled % kMillisecondsPerSecond) +
                                        within_ms * static_cast<double>(kTicksPerSecond)) /
                                           static_cast<double>(kMillisecondsPerSecond);
    const uint32_t since = whole_units - timing.previous_clock;
    // A clock read before the step (a reset or a wrap) counts as no time.
    const double units = since > kTicksPerSecond ? 0.0 : static_cast<double>(since) + unit_fraction;
    double alpha = units;
    if (timing.actual_rate < normal_game_speed)
        alpha = static_cast<double>(timing.remainder) +
                units * static_cast<double>(timing.actual_rate) * kSpeedTenths;
    alpha = std::clamp(alpha, 0.0, 1.0);
    if (!batch)
        alpha = std::max(alpha, static_cast<double>(presentation.alpha));
    presentation.alpha = static_cast<float>(alpha);
    return presentation.alpha;
}

double scroll_distance(uint8_t scroll_speed, uint64_t elapsed_ns) noexcept {
    const double units = static_cast<double>(elapsed_ns) *
                         static_cast<double>(oa::ui::hud::kScrollClockHz) /
                         static_cast<double>(kNanosecondsPerSecond);
    const double distance = static_cast<double>(scroll_speed) * units;
    // A frame of a whole clock unit or more scrolls at most kMaxScrollStep,
    // as each 3.1c frame does; a shorter frame at most its share of it.
    const double cap = static_cast<double>(oa::ui::hud::kMaxScrollStep) * std::min(units, 1.0);
    return std::min(distance, cap);
}

void note_frame_measure(
    FrameStatsWindow& window, FrameMeasure measure, uint64_t elapsed_ns
) noexcept {
    auto& spread = window.measuring[static_cast<std::size_t>(measure)];
    const TimeSeverity severity = time_severity(elapsed_ns, window.allowance_ns);
    if (spread.count == 0 || elapsed_ns < spread.least_ns) {
        spread.least_ns = elapsed_ns;
        spread.least_severity = severity;
    }
    if (spread.count == 0 || elapsed_ns > spread.most_ns) {
        spread.most_ns = elapsed_ns;
        spread.most_severity = severity;
    }
    spread.total_ns += elapsed_ns;
    spread.allowance_total_ns += window.allowance_ns;
    ++spread.count;
    if (measure == FrameMeasure::frame)
        note_frame_history(window.history, elapsed_ns, severity);
}

bool roll_frame_stats(FrameStatsWindow& window, uint64_t now_ns) noexcept {
    if (!window.started) {
        window.started = true;
        window.measuring_since_ns = now_ns;
        window.measuring = {};
        return false;
    }
    if (now_ns < window.measuring_since_ns + kNanosecondsPerSecond)
        return false;
    window.shown = window.measuring;
    window.shown_span_ns = now_ns - window.measuring_since_ns;
    window.measuring = {};
    window.measuring_since_ns = now_ns;
    return true;
}

void note_frame_history(
    FrameHistory& history, uint64_t elapsed_ns, TimeSeverity severity
) noexcept {
    const uint64_t start = history.end_ns;
    history.end_ns += elapsed_ns;
    uint64_t first = middles_by(start);
    uint64_t past_last = middles_by(history.end_ns);
    if (first == past_last) {
        first = graph_column(history.end_ns);
        past_last = first + 1;
    }
    // The columns the frame moves the graph on by start empty.
    const uint64_t last = past_last - 1;
    if (last > history.newest_column) {
        const uint64_t entered =
            std::min<uint64_t>(last - history.newest_column, kFrameGraphColumns);
        for (uint64_t back = 0; back < entered; ++back) {
            const auto slot = static_cast<std::size_t>((last - back) % kFrameGraphColumns);
            history.longest_ns[slot] = 0;
            history.severity[slot] = TimeSeverity::none;
        }
        history.newest_column = last;
    }
    if (history.newest_column + 1 > kFrameGraphColumns)
        first = std::max<uint64_t>(first, history.newest_column + 1 - kFrameGraphColumns);
    for (uint64_t column = first; column < past_last; ++column) {
        const auto slot = static_cast<std::size_t>(column % kFrameGraphColumns);
        if (history.severity[slot] != TimeSeverity::none && history.longest_ns[slot] >= elapsed_ns)
            continue;
        history.longest_ns[slot] = elapsed_ns;
        history.severity[slot] = severity;
    }
}

FrameColumn frame_history_column(const FrameHistory& history, std::size_t age) noexcept {
    if (age >= kFrameGraphColumns || age > history.newest_column)
        return {};
    const auto slot = static_cast<std::size_t>((history.newest_column - age) % kFrameGraphColumns);
    if (history.severity[slot] == TimeSeverity::none)
        return {};
    return {history.longest_ns[slot], history.severity[slot]};
}

uint64_t frame_budget_ns(uint32_t frames_per_second) noexcept {
    return frames_per_second == 0 ? kTickBudgetNs : kNanosecondsPerSecond / frames_per_second;
}

uint64_t frame_allowance_ns(uint32_t frames_per_second, FrameWait wait) noexcept {
    return frame_budget_ns(frames_per_second) +
           (wait == FrameWait::idle ? kIdleWaitSlackNs : kFrameBudgetSlackNs);
}

TimeSeverity time_severity(uint64_t elapsed_ns, uint64_t allowance_ns) noexcept {
    if (elapsed_ns <= allowance_ns)
        return TimeSeverity::within_frame;
    return elapsed_ns <= kTickBudgetNs ? TimeSeverity::within_tick : TimeSeverity::over_tick;
}

std::string_view whole_characters(std::string_view text, std::size_t bytes) noexcept {
    if (text.size() <= bytes)
        return text;
    std::size_t length = bytes;
    while (length > 0 &&
           (static_cast<unsigned char>(text[length]) & kUtf8LeadMask) == kUtf8Continuation)
        --length;
    return text.substr(0, length);
}

std::string_view FrameStatsText::view() const noexcept {
    const auto end = std::find(text.begin(), text.end(), '\0');
    return {text.data(), static_cast<std::size_t>(end - text.begin())};
}

void set_display_row(FrameStatsRow& row, const FrameStatsDisplay& display) noexcept {
    row.kind = FrameStatsRowKind::display;
    row.label = {};
    row.note = {};
    if (display.screen == FrameStatsScreen::none) {
        set_text(row.label, "no window");
        return;
    }
    const std::string_view screen = display.screen == FrameStatsScreen::window      ? "window"
                                    : display.screen == FrameStatsScreen::exclusive ? "exclusive"
                                                                                    : "full screen";
    std::snprintf(
        row.label.text.data(),
        row.label.text.size(),
        "%.*s %dx%d",
        static_cast<int>(screen.size()),
        screen.data(),
        static_cast<int>(display.frame_width),
        static_cast<int>(display.frame_height)
    );
    // The note's parts, each left out when it is not known.
    std::array<char, kModeTextBytes> mode{};
    if (display.mode_width > 0 && display.mode_height > 0) {
        const long rate = std::lround(static_cast<double>(display.refresh_rate));
        if (rate > 0)
            std::snprintf(
                mode.data(),
                mode.size(),
                "%dx%d@%ld",
                static_cast<int>(display.mode_width),
                static_cast<int>(display.mode_height),
                rate
            );
        else
            std::snprintf(
                mode.data(),
                mode.size(),
                "%dx%d",
                static_cast<int>(display.mode_width),
                static_cast<int>(display.mode_height)
            );
    }
    std::array<char, kFrameStatsTextBytes> scale{};
    if (display.display_scale > 0.0F)
        write_scale(display.display_scale, scale.data(), scale.size());
    const char* const gap = mode[0] != '\0' && scale[0] != '\0' ? " " : "";
    std::array<char, kNoteTextBytes> note{};
    std::snprintf(note.data(), note.size(), "%s%s%s", mode.data(), gap, scale.data());
    set_text(row.note, note.data());
}

FrameStatsTable frame_stats_table(
    const FrameStatsWindow& window,
    const FrameStatsNotes& notes,
    const FrameStatsRenderer& renderer,
    const FrameStatsDisplay& display
) noexcept {
    const bool shown = window.shown_span_ns != 0;
    const uint32_t rate = notes.paced_frames_per_second;
    FrameStatsTable table = table_rows();

    auto& frames_row = table.rows[kRateRow];
    const auto& frames = spread_of(window.shown, FrameMeasure::frame);
    auto& frames_value = frames_row.values[kFrameStatsMeanColumn];
    if (shown) {
        set_number(frames_value, per_second(frames.count, window.shown_span_ns));
        if (frames.count != 0)
            frames_value.severity = time_severity(
                window.shown_span_ns / frames.count, frames.allowance_total_ns / frames.count
            );
    } else {
        set_text(frames_value, kNoMeasure);
    }
    auto& pacing = frames_row.note.text;
    if (rate == 0)
        set_text(frames_row.note, "no limit");
    else if (rate < notes.max_frames_per_second || notes.max_frames_per_second == 0)
        std::snprintf(pacing.data(), pacing.size(), "idle %u", rate);
    else
        std::snprintf(pacing.data(), pacing.size(), "limit %u", rate);

    for (std::size_t measure = 0; measure < kFrameMeasureCount; ++measure) {
        auto& row = table.rows[kFirstMeasureRow + measure];
        set_measure_row(row, window.shown[measure], shown);
        if (shown && measure == static_cast<std::size_t>(FrameMeasure::tick))
            std::snprintf(
                row.note.text.data(),
                row.note.text.size(),
                "%llu/s",
                static_cast<unsigned long long>(
                    per_second(window.shown[measure].count, window.shown_span_ns)
                )
            );
    }

    auto& units = table.rows[kCountRow];
    set_number(units.values[kFrameStatsMeanColumn], notes.units_drawn);
    std::snprintf(
        units.note.text.data(),
        units.note.text.size(),
        "%u between ticks",
        notes.units_between_ticks
    );
    set_renderer_row(table.rows[kRendererRow], renderer);
    set_display_row(table.rows[kDisplayRow], display);
    return table;
}

FrameStatsTable frame_stats_widest_table() noexcept {
    // Zeros stand for any digit; a font whose digits differ in width may
    // draw a text a pixel or two past the room kept for it.
    constexpr std::string_view kWidestTime = "000.00";
    FrameStatsTable table = table_rows();
    set_text(table.rows[kRateRow].values[kFrameStatsMeanColumn], "0000");
    set_text(table.rows[kRateRow].note, "limit 0000");
    for (std::size_t measure = 0; measure < kFrameMeasureCount; ++measure)
        for (auto& value : table.rows[kFirstMeasureRow + measure].values)
            set_text(value, kWidestTime);
    set_text(
        table.rows[kFirstMeasureRow + static_cast<std::size_t>(FrameMeasure::tick)].note, "000/s"
    );
    set_text(table.rows[kCountRow].values[kFrameStatsMeanColumn], "0000");
    set_text(table.rows[kCountRow].note, "0000 between ticks");
    // The renderer and display rows stay empty: the panel cuts their texts
    // to its width.
    return table;
}

uint64_t steady_now_ns() noexcept {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch()
    )
                                     .count());
}

} // namespace oa::app::frame_pacing
