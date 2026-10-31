// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The parts of --bench that hold no game: the view file it reads, the spread
// of the times it measures and the lines it prints them in
// (docs/command-line.md#--bench). Runtime::run_bench (runtime_bench.cpp)
// plays the run.
#pragma once

#include "bench_timers.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app::bench {

/// What a bench view file sets: the run's name, the match, the camera and the
/// span the bench measures. Each member is the value a file that leaves its
/// key out gets.
struct View {
    std::string label;           ///< the run's name in its lines; the file's stem
    std::filesystem::path stage; ///< a stage played into the match; empty for none
    uint16_t players = 2;        ///< the skirmish's players
    double menu_seconds = 1.0;   ///< seconds on the main menu before SINGLE
    uint32_t warmup_ticks = 600; ///< the match tick the measured span starts at
    double measure_seconds = 25; ///< the longest measured span, in seconds
    /// The measured span in match ticks, however long they take, within
    /// measure_seconds; 0 measures for measure_seconds.
    uint32_t measure_ticks{};
    double shot_at = 12;      ///< seconds into the span of the SHOT line
    uint32_t shot_ticks{};    ///< ticks into the span of the SHOT line; 0 for shot_at
    double centre_x = 2048;   ///< the map pixel the view centres on, across
    double centre_z = 2048;   ///< and down
    double yaw{};             ///< the camera's turn at the start, in degrees
    double pitch{};           ///< its tilt, in degrees; yaw and pitch 0 is the classic view
    double zoom = 1;          ///< the battlefield's zoom; 0 for the farthest out
    double turn_rate{};       ///< degrees of yaw a second through the span
    double hold_after{};      ///< seconds the match plays on after the span
    bool stage_timers = true; ///< whether the stage timers measure
    bool frame_log{};         ///< whether a FRAME line is printed for each frame of the span
};

/// Reads a view file's text: one "KEY VALUE" a line; blank lines and lines
/// that begin with # are skipped.
///
/// Every key may be left out. A stage's path is kept as written. A key the
/// bench does not know, a number that does not read, one outside its range
/// and a key given twice are errors.
///
/// @param text the file's text
/// @param[in,out] view the values before the file; those its keys name are
///        replaced
/// @param[out] error what was wrong and on which line, when the text is
///        refused
/// @return true when every line read
[[nodiscard]] bool parse_view(std::string_view text, View& view, std::string& error);

/// Reads a view file. The label is the file's stem unless the file names
/// one, and a stage path that is not absolute is taken from the file's
/// folder.
///
/// @param file the view file
/// @param[out] view what the file sets
/// @param[out] error what was wrong, when the file cannot be read or is refused
/// @return true when the file read
[[nodiscard]] bool read_view(const std::filesystem::path& file, View& view, std::string& error);

/// The spread of a measure over the frames of the span, in milliseconds.
struct Spread {
    double least{};
    double mean{};
    double p50{}; ///< the median
    double p95{};
    double p99{};
    double low1{}; ///< the mean of the slowest 1% of the values, at least one
    double most{};
};

/// Returns the spread of a measure's values. The n-th percentile of N values
/// is the value at position N * n / 100, rounded down and counted from 0, of
/// the values sorted from least to most, or the most when that is past the
/// end. The slowest 1% are the largest ceil(N / 100) values.
///
/// @param values the values, in any order
/// @return the spread; all zero when there are no values
[[nodiscard]] Spread spread(std::span<const double> values);

/// Returns the frames whose values are a spread's slowest 1% (Spread::low1):
/// the largest ceil(N / 100) values, of equal values the later frames'.
///
/// @param values the values, one a frame
/// @return the frames' positions in values, in order
[[nodiscard]] std::vector<std::size_t> slowest_frames(std::span<const double> values);

/// Returns a frame rate for a frame time.
///
/// @param milliseconds the frame's time
/// @return frames a second; 0 for a time of 0
[[nodiscard]] double frames_per_second(double milliseconds);

/// Returns a MEASURE line: "bench: MEASURE NAME min M avg A p50 P p95 P p99 P
/// low1 L max X", the times in milliseconds to two places.
///
/// @param name the measure: frame, work, sim(per frame), draw or present
/// @param values its spread
/// @return the line, with no newline
[[nodiscard]] std::string measure_line(std::string_view name, const Spread& values);

/// Returns a STAGE line: "bench: STAGE NAME MS", the stage's mean
/// milliseconds a frame to two places, its name indented two spaces when it
/// is part of the stage before it.
///
/// @param stage the stage
/// @param milliseconds its time over the span
/// @param frames the frames of the span
/// @return the line, with no newline
[[nodiscard]] std::string
stage_line(bench_timers::Stage stage, double milliseconds, std::size_t frames);

/// Returns a SLOW STAGE line: "bench: SLOW STAGE NAME MS", laid out as a
/// STAGE line, the stage's mean milliseconds over the slowest 1% of frames.
///
/// @param stage the stage
/// @param milliseconds its time over those frames
/// @param frames how many they are
/// @return the line, with no newline
[[nodiscard]] std::string
slow_stage_line(bench_timers::Stage stage, double milliseconds, std::size_t frames);

} // namespace oa::app::bench
