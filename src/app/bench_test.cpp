// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The parts of --bench that hold no game: a view file's keys and defaults,
// the keys and values it refuses, a stage taken from the file's folder and
// the label from its name; requested and started Enhanced Mode rules; the
// spread of a measure, its percentiles and its
// slowest 1% and which frames those are; the MEASURE, STAGE and SLOW STAGE
// lines; simulation frames' tick boundaries and existing step maxima; and
// the stage timers, which sum nothing while off, and time stages,
// parts and the atlas's counts while on, frame by frame.
#include "bench.hpp"
#include "bench_timers.hpp"

#include "oa/platform/files.hpp"
#include "oa/test/check.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace bench = oa::app::bench;
namespace timers = oa::app::bench_timers;
using timers::Stage;

/// How near two times must be to count as equal, in milliseconds.
constexpr double tolerance = 1e-9;

/// Returns whether two numbers are equal within tolerance.
///
/// @param left one number
/// @param right the other
/// @return true when they are within tolerance
[[nodiscard]] bool near(double left, double right) {
    return std::abs(left - right) <= tolerance;
}

/// Returns the error a view's text is refused with, or "" when it reads.
///
/// @param text the view file's text
/// @return the error
[[nodiscard]] std::string refusal(std::string_view text) {
    bench::View view;
    std::string error;
    return bench::parse_view(text, view, error) ? std::string{} : error;
}

/// Every key of a view file is read, and a view without them keeps the
/// defaults the bench's views were measured with.
void view_reads_every_key() {
    bench::View defaults;
    std::string error;
    OA_CHECK(bench::parse_view("# nothing but a comment\n\n", defaults, error));
    OA_CHECK(defaults.players == 2 && defaults.warmup_ticks == 600);
    OA_CHECK(near(defaults.measure_seconds, 25) && defaults.measure_ticks == 0);
    OA_CHECK(near(defaults.zoom, 1) && near(defaults.yaw, 0) && near(defaults.pitch, 0));
    OA_CHECK(defaults.stage_timers && defaults.stage.empty());
    OA_CHECK(defaults.warmup_frames == 0 && defaults.measure_frames == 0);
    OA_CHECK(defaults.expected_live_units == 0 && !defaults.require_paused);
    OA_CHECK(defaults.expected_initial_units == 0 && defaults.expected_initial_per_side == 0);
    OA_CHECK(defaults.expected_output_width == 0 && defaults.expected_output_height == 0);
    OA_CHECK(defaults.display_number == 0 && !defaults.require_gameplay);
    OA_CHECK(defaults.rules_version == 0);

    bench::View view;
    const char* text = "# the low view\n"
                       "label low-across\r\n"
                       "stage battle.stage\n"
                       "players 3\n"
                       "rules_version 6\n"
                       "menu_seconds 0.5\n"
                       "warmup_ticks 60\n"
                       "measure_seconds 150\n"
                       "measure_ticks 750\n"
                       "  shot_at 12\n"
                       "shot_ticks 360\n"
                       "centre_x 1980\n"
                       "centre_z 2080\n"
                       "yaw 20\n"
                       "pitch -53\n"
                       "zoom 0\n"
                       "turn_rate 6\n"
                       "hold_after 2\n"
                       "stage_timers 0\n"
                       "frame_log 1";
    OA_CHECK(bench::parse_view(text, view, error));
    OA_CHECK(view.label == "low-across");
    OA_CHECK(oa::platform::path_to_generic_utf8(view.stage) == "battle.stage");
    OA_CHECK(view.players == 3 && near(view.menu_seconds, 0.5));
    OA_CHECK(view.rules_version == 6);
    OA_CHECK(view.warmup_ticks == 60 && view.measure_ticks == 750 && view.shot_ticks == 360);
    OA_CHECK(near(view.measure_seconds, 150) && near(view.shot_at, 12));
    OA_CHECK(near(view.centre_x, 1980) && near(view.centre_z, 2080));
    OA_CHECK(near(view.yaw, 20) && near(view.pitch, -53) && near(view.zoom, 0));
    OA_CHECK(near(view.turn_rate, 6) && near(view.hold_after, 2) && !view.stage_timers);
    OA_CHECK(view.frame_log && !defaults.frame_log);

    bench::View paused;
    OA_CHECK(
        bench::parse_view(
            "warmup_ticks 0\nwarmup_frames 30\nmeasure_frames 60\n"
            "expected_live_units 20000\nrequire_paused 1\n",
            paused,
            error
        )
    );
    OA_CHECK(paused.warmup_frames == 30 && paused.measure_frames == 60);
    OA_CHECK(paused.expected_live_units == 20000 && paused.require_paused);
}

/// An absent or zero rules request keeps the version selected by the match;
/// an explicit supported preset requires Enhanced Mode before startup.
void rules_requests_preserve_defaults_and_require_enhanced_mode() {
    bench::View defaults;
    std::string error;
    OA_CHECK(bench::parse_view("", defaults, error));
    OA_CHECK(defaults.rules_version == 0);
    OA_CHECK(bench::validate_rules_request(defaults, false, error));
    OA_CHECK(bench::validate_rules_request(defaults, true, error));
    OA_CHECK(bench::validate_started_rules(defaults, 0, error));
    OA_CHECK(bench::validate_started_rules(defaults, 4, error));
    OA_CHECK(defaults.rules_version == 0);

    for (const uint32_t version : std::array<uint32_t, 9>{0, 1, 2, 3, 4, 5, 6, 7, 8}) {
        bench::View view;
        OA_CHECK(bench::parse_view("rules_version " + std::to_string(version), view, error));
        OA_CHECK(view.rules_version == version);
        OA_CHECK(bench::validate_rules_request(view, true, error));
        OA_CHECK(bench::validate_rules_request(view, false, error) == (version == 0));
        if (version != 0)
            OA_CHECK(error == "rules_version requires Enhanced Mode");
        OA_CHECK(bench::validate_started_rules(view, version, error));
        OA_CHECK(view.rules_version == version);
    }

    bench::View explicit_zero;
    OA_CHECK(bench::parse_view("rules_version 0", explicit_zero, error));
    OA_CHECK(bench::validate_started_rules(explicit_zero, 0, error));
    OA_CHECK(bench::validate_started_rules(explicit_zero, 4, error));
    OA_CHECK(bench::validate_started_rules(explicit_zero, 6, error));
    OA_CHECK(bench::validate_started_rules(explicit_zero, 7, error));
    OA_CHECK(bench::validate_started_rules(explicit_zero, 8, error));
    OA_CHECK(explicit_zero.rules_version == 0);
}

/// An explicit version must equal the rules of the started match. Unsupported
/// requests and unsupported actual rules are refused independently.
void started_rules_must_match_explicit_requests() {
    std::string error;
    for (const uint32_t requested : std::array<uint32_t, 8>{1, 2, 3, 4, 5, 6, 7, 8}) {
        bench::View view;
        view.rules_version = requested;
        for (const uint32_t actual : std::array<uint32_t, 9>{0, 1, 2, 3, 4, 5, 6, 7, 8}) {
            OA_CHECK(bench::validate_started_rules(view, actual, error) == (requested == actual));
            if (requested != actual)
                OA_CHECK(error == "the skirmish's Enhanced Mode rules differ from rules_version");
            OA_CHECK(view.rules_version == requested);
        }
    }

    bench::View unsupported;
    unsupported.rules_version = 9;
    OA_CHECK(!bench::validate_rules_request(unsupported, true, error));
    OA_CHECK(error == "rules_version is unsupported");
    OA_CHECK(!bench::validate_rules_request(unsupported, false, error));
    OA_CHECK(error == "rules_version is unsupported");
    for (const uint32_t requested : std::array<uint32_t, 9>{0, 1, 2, 3, 4, 5, 6, 7, 8}) {
        bench::View view;
        view.rules_version = requested;
        OA_CHECK(!bench::validate_started_rules(view, 9, error));
        OA_CHECK(error == "the skirmish's Enhanced Mode rules version is unsupported");
        OA_CHECK(view.rules_version == requested);
    }
}

/// A rules version is a supported nonnegative whole number and may be named
/// only once, with the refusal attached to its input line.
void view_refuses_invalid_rules_versions() {
    for (const std::string_view value :
         std::array<std::string_view, 4>{"9", "-1", "4.5", "4294967296"})
        OA_CHECK(
            refusal("rules_version " + std::string(value)) ==
            "line 1: rules_version takes 0 or a supported Enhanced Mode rules version"
        );
    OA_CHECK(refusal("rules_version 4\nrules_version 6") == "line 2: rules_version is given twice");
}

/// A combat run verifies its starting armies without requiring casualties
/// to keep the same live population throughout its tick span.
void gameplay_view_checks_only_the_starting_population() {
    bench::View view;
    std::string error;
    OA_CHECK(
        bench::parse_view(
            "require_gameplay 1\nwarmup_ticks 120\nmeasure_ticks 900\nmeasure_seconds 300\n"
            "expected_initial_units 20000\nexpected_initial_per_side 10000\n"
            "expected_output_width 1920\nexpected_output_height 1080\ndisplay_number 2\n",
            view,
            error
        )
    );
    OA_CHECK(view.require_gameplay && !view.require_paused);
    OA_CHECK(view.expected_initial_units == 20000 && view.expected_initial_per_side == 10000);
    OA_CHECK(view.expected_live_units == 0);
    OA_CHECK(view.warmup_ticks == 120 && view.measure_ticks == 900);
    OA_CHECK(near(view.measure_seconds, 300));
    OA_CHECK(view.expected_output_width == 1920 && view.expected_output_height == 1080);
    OA_CHECK(view.display_number == 2 && near(view.turn_rate, 0));

    const std::string invalid =
        "require_gameplay needs an unpaused tick span, fixed camera and initial-only counts";
    OA_CHECK(refusal("require_gameplay 1") == invalid);
    OA_CHECK(
        refusal("require_gameplay 1\nmeasure_ticks 900\nexpected_live_units 20000") == invalid
    );
    OA_CHECK(refusal("require_gameplay 1\nmeasure_ticks 900\nturn_rate 1") == invalid);
    OA_CHECK(refusal("require_gameplay 1\nrequire_paused 1\nwarmup_ticks 0") == invalid);
    OA_CHECK(refusal("require_gameplay 2") == "line 1: require_gameplay takes 0 or 1");
    OA_CHECK(!refusal("expected_initial_units -1").empty());
    OA_CHECK(!refusal("expected_initial_per_side 1.5").empty());
    OA_CHECK(!refusal("display_number -1").empty());
    OA_CHECK(
        refusal("expected_output_width 1920") ==
        "expected_output_width and expected_output_height must both be given"
    );
    OA_CHECK(
        refusal("expected_output_height 1080") ==
        "expected_output_width and expected_output_height must both be given"
    );
}

/// A key the bench does not know, a value that does not read or is out of
/// range, and a key given twice are refused with their line.
void view_refuses_what_it_cannot_read() {
    OA_CHECK(refusal("label a\nspeed 3\n") == "line 2: the bench has no key speed");
    OA_CHECK(refusal("yaw twenty") == "line 1: yaw takes a number");
    OA_CHECK(refusal("yaw 20deg") == "line 1: yaw takes a number");
    OA_CHECK(refusal("yaw inf") == "line 1: yaw takes a number");
    OA_CHECK(refusal("warmup_ticks -5") == "line 1: warmup_ticks takes a whole number of ticks");
    OA_CHECK(refusal("warmup_ticks 1.5") == "line 1: warmup_ticks takes a whole number of ticks");
    OA_CHECK(refusal("measure_seconds -1") == "line 1: measure_seconds takes seconds, 0 or more");
    OA_CHECK(refusal("players 1") == "line 1: players takes 2 to 10");
    OA_CHECK(refusal("players 11") == "line 1: players takes 2 to 10");
    OA_CHECK(refusal("zoom -1") == "line 1: zoom takes 0 for the farthest out, or a zoom above 0");
    OA_CHECK(refusal("stage_timers yes") == "line 1: stage_timers takes 0 or 1");
    OA_CHECK(refusal("frame_log 2") == "line 1: frame_log takes 0 or 1");
    OA_CHECK(refusal("warmup_frames -1") == "line 1: warmup_frames takes a whole number of frames");
    OA_CHECK(
        refusal("measure_frames 1.5") == "line 1: measure_frames takes a whole number of frames"
    );
    OA_CHECK(
        refusal("expected_live_units -1") ==
        "line 1: expected_live_units takes a whole number of units"
    );
    OA_CHECK(refusal("require_paused 2") == "line 1: require_paused takes 0 or 1");
    OA_CHECK(
        refusal("require_paused 1") ==
        "require_paused needs warmup_ticks, measure_ticks and turn_rate set to 0"
    );
    OA_CHECK(
        refusal("require_paused 1\nwarmup_ticks 0\nmeasure_ticks 1") ==
        "require_paused needs warmup_ticks, measure_ticks and turn_rate set to 0"
    );
    OA_CHECK(
        refusal("require_paused 1\nwarmup_ticks 0\nturn_rate 1") ==
        "require_paused needs warmup_ticks, measure_ticks and turn_rate set to 0"
    );
    OA_CHECK(refusal("\n\nlabel") == "line 3: label has no value");
    OA_CHECK(refusal("yaw 1\nyaw 2") == "line 2: yaw is given twice");
}

/// read_view names the run after its file and takes a relative stage from
/// the file's folder; an absolute stage stays as it is.
void view_file_takes_its_folder() {
    const auto folder = std::filesystem::temp_directory_path() / "oa-app-bench-test";
    std::filesystem::create_directories(folder);
    const auto file = folder / "view-turning.bench";
    {
        std::ofstream out(file, std::ios::binary);
        out << "stage battle.stage\nturn_rate 6\n";
    }
    bench::View view;
    std::string error;
    OA_CHECK(bench::read_view(file, view, error));
    OA_CHECK(view.label == "view-turning");
    OA_CHECK(view.stage == folder / "battle.stage");
    OA_CHECK(near(view.turn_rate, 6));

    const auto absolute = folder / "elsewhere" / "idle.stage";
    {
        std::ofstream out(file, std::ios::binary);
        out << "label named\nstage " << oa::platform::path_to_utf8(absolute) << "\n";
    }
    OA_CHECK(bench::read_view(file, view, error));
    OA_CHECK(view.label == "named" && view.stage == absolute);

    {
        std::ofstream out(file, std::ios::binary);
        out << "zoom far\n";
    }
    OA_CHECK(!bench::read_view(file, view, error));
    OA_CHECK(error == oa::platform::path_to_utf8(file) + ": line 1: zoom takes a number");
    OA_CHECK(view.label == "named");

    std::filesystem::remove_all(folder);
    OA_CHECK(!bench::read_view(file, view, error));
    OA_CHECK(error == "cannot read the bench file " + oa::platform::path_to_utf8(file));
}

/// The spread of a measure: its least, mean and most, its percentiles at
/// N * n / 100 of the sorted values, and the mean of its slowest 1%.
void spread_takes_percentiles_and_the_slowest() {
    const bench::Spread none = bench::spread({});
    OA_CHECK(near(none.mean, 0) && near(none.p99, 0) && near(none.low1, 0));

    // 1 to 200 ms, given in reverse: the slowest 1% are 199 and 200.
    std::vector<double> values;
    for (int value = 200; value >= 1; --value)
        values.push_back(value);
    const auto spread = bench::spread(values);
    OA_CHECK(near(spread.least, 1) && near(spread.most, 200));
    OA_CHECK(near(spread.mean, 100.5));
    OA_CHECK(near(spread.p50, 101)); // position 100
    OA_CHECK(near(spread.p95, 191)); // position 190
    OA_CHECK(near(spread.p99, 199)); // position 198
    OA_CHECK(near(spread.low1, 199.5));

    // 495 frames, the run the old bench's p95 was taken from: position 470.
    std::vector<double> frames(495);
    for (std::size_t index = 0; index < frames.size(); ++index)
        frames[index] = static_cast<double>(index);
    const auto many = bench::spread(frames);
    OA_CHECK(near(many.p95, 470) && near(many.p99, 490) && near(many.p50, 247));
    // ceil(495 / 100) = 5 slowest: 490 to 494.
    OA_CHECK(near(many.low1, 492));

    // A single value is every figure; a percentile never passes the end.
    const std::array<double, 1> one{40};
    const auto single = bench::spread(one);
    OA_CHECK(near(single.least, 40) && near(single.p99, 40) && near(single.low1, 40));

    OA_CHECK(near(bench::frames_per_second(50), 20) && near(bench::frames_per_second(0), 0));
}

/// The slowest 1% of frames are the ones Spread::low1 takes the mean of,
/// in the order the frames came; of equal times the later frames'.
void slowest_frames_are_the_slowest_one_percent() {
    OA_CHECK(bench::slowest_frames({}).empty());
    const std::array<double, 1> one{40};
    OA_CHECK(bench::slowest_frames(one) == std::vector<std::size_t>{0});
    // 201 frames: ceil(201 / 100) = 3 slowest, frames 7, 50 and 120.
    std::vector<double> frames(201, 20.0);
    frames[120] = 61;
    frames[7] = 45;
    frames[50] = 45;
    frames[90] = 44;
    OA_CHECK((bench::slowest_frames(frames) == std::vector<std::size_t>{7, 50, 120}));
    double total = 0;
    for (const std::size_t index : bench::slowest_frames(frames))
        total += frames[index];
    OA_CHECK(near(total / 3, bench::spread(frames).low1));
    // Of equal times, the later frames are taken.
    const std::vector<double> level(150, 30.0);
    OA_CHECK((bench::slowest_frames(level) == std::vector<std::size_t>{148, 149}));
}

/// The MEASURE and STAGE lines keep the old bench's layout, with the median,
/// the 99th percentile and the slowest 1% added.
void lines_keep_their_layout() {
    const bench::Spread spread{40.96, 50.5, 49.875, 58.41, 70.0, 75.2, 84.36};
    OA_CHECK(
        bench::measure_line("frame", spread) ==
        "bench: MEASURE frame            min    40.96 avg    50.50 p50    49.88 p95    58.41 p99 "
        "   70.00 low1    75.20 max    84.36"
    );
    OA_CHECK(
        bench::stage_line(Stage::frame, 4677.0, 100) ==
        "bench: STAGE frame(render_match_surface)             46.77"
    );
    OA_CHECK(
        bench::stage_line(Stage::terrain, 110.0, 100) ==
        "bench: STAGE   terrain                                1.10"
    );
    OA_CHECK(
        bench::stage_line(Stage::match_plan, 5.0, 0) ==
        "bench: STAGE ds_plan(draw list)                       0.00"
    );
    OA_CHECK(
        bench::slow_stage_line(Stage::terrain, 137.0, 10) ==
        "bench: SLOW STAGE   terrain                               13.70"
    );
    // Every stage has a name, and the names are told apart.
    for (std::size_t first = 0; first < timers::stage_count; ++first) {
        OA_CHECK(!timers::stage_names[first].name.empty());
        for (std::size_t second = first + 1; second < timers::stage_count; ++second)
            OA_CHECK(timers::stage_names[first].name != timers::stage_names[second].name);
    }
}

/// A zero-step frame differs from a catch-up frame whose simulation wall
/// sum is long even though no individual step is long.
void simulation_frames_separate_catchup_from_slow_steps() {
    bench::SimulationFrame idle{};
    idle.tick_before = idle.tick_after = 900;
    {
        const bench::SimulationFrameScope recording(&idle);
    }
    OA_CHECK(
        bench::simulation_frame_line(0, 0, idle) ==
        "bench: SIM_FRAME 0 sim_ms 0.000 tick_before 900 tick_after 900 ticks 0 steps 0 "
        "max_step_ms 0.000 max_step_tick 0"
    );

    bench::SimulationFrame catchup{};
    catchup.tick_before = 900;
    {
        const bench::SimulationFrameScope recording(&catchup);
        bench::note_simulation_step(901, 15'000'000);
        bench::note_simulation_step(902, 30'000'000);
        bench::note_simulation_step(903, 15'000'000);
        bench::note_simulation_step(904, 15'000'000);
        bench::note_simulation_step(905, 15'000'000);
    }
    catchup.tick_after = 905;
    OA_CHECK(
        bench::simulation_frame_line(1, 90, catchup) ==
        "bench: SIM_FRAME 1 sim_ms 90.000 tick_before 900 tick_after 905 ticks 5 steps 5 "
        "max_step_ms 30.000 max_step_tick 902"
    );

    bench::SimulationFrame slow{};
    slow.tick_before = 905;
    {
        const bench::SimulationFrameScope recording(&slow);
        bench::note_simulation_step(906, 90'000'000);
    }
    slow.tick_after = 906;
    OA_CHECK(
        bench::simulation_frame_line(2, 90, slow) ==
        "bench: SIM_FRAME 2 sim_ms 90.000 tick_before 905 tick_after 906 ticks 1 steps 1 "
        "max_step_ms 90.000 max_step_tick 906"
    );
    bench::note_simulation_step(907, 120'000'000);
    OA_CHECK(slow.steps == 1 && slow.slowest_tick == 906 && slow.slowest_ns == 90'000'000);
}

/// Tick deltas wrap, a measured zero duration still names its tick, and an
/// extension's multi-tick step remains distinct from the native tick delta.
void simulation_frames_preserve_tick_and_step_boundaries() {
    bench::SimulationFrame wrapped{};
    wrapped.tick_before = std::numeric_limits<uint32_t>::max() - 1;
    {
        const bench::SimulationFrameScope recording(&wrapped);
        bench::note_simulation_step(std::numeric_limits<uint32_t>::max(), 5'000'000);
        bench::note_simulation_step(0, 10'000'000);
        bench::note_simulation_step(1, 10'000'000);
    }
    wrapped.tick_after = 1;
    OA_CHECK(
        bench::simulation_frame_line(3, 25, wrapped) ==
        "bench: SIM_FRAME 3 sim_ms 25.000 tick_before 4294967294 tick_after 1 ticks 3 steps 3 "
        "max_step_ms 10.000 max_step_tick 0"
    );

    bench::SimulationFrame extension{};
    extension.tick_before = 40;
    {
        const bench::SimulationFrameScope recording(&extension);
        bench::note_simulation_step(42, 0);
    }
    extension.tick_after = 42;
    OA_CHECK(
        bench::simulation_frame_line(4, 0, extension) ==
        "bench: SIM_FRAME 4 sim_ms 0.000 tick_before 40 tick_after 42 ticks 2 steps 1 "
        "max_step_ms 0.000 max_step_tick 42"
    );
}

/// Nested measurements remain in their enclosing frame's maximum and
/// count, and unwinding restores recording without leaking a stack pointer.
void simulation_frame_scopes_restore_their_recorders() {
    bench::SimulationFrame outer{}, inner{};
    {
        const bench::SimulationFrameScope recording(&outer);
        bench::note_simulation_step(10, 2);
        try {
            const bench::SimulationFrameScope nested(&inner);
            bench::note_simulation_step(11, 7);
            throw std::runtime_error("end the nested frame");
        } catch (const std::runtime_error&) {
        }
        bench::note_simulation_step(12, 7);
        {
            const bench::SimulationFrameScope disabled(nullptr);
            bench::note_simulation_step(13, 100);
        }
        {
            const bench::SimulationFrameScope same(&outer);
            bench::note_simulation_step(14, 3);
        }
    }
    OA_CHECK(inner.steps == 1 && inner.slowest_ns == 7 && inner.slowest_tick == 11);
    OA_CHECK(outer.steps == 4 && outer.slowest_ns == 7 && outer.slowest_tick == 11);
    bench::note_simulation_step(15, 100);
    OA_CHECK(outer.steps == 4 && inner.steps == 1);
}

/// While the timers are off nothing is summed; on, a span times its stage,
/// packing is charged to the innermost span, marks follow one another, and
/// a reset clears the sums but keeps the last scene and ground.
void timers_sum_only_while_on() {
    timers::reset(false);
    {
        timers::Span span(Stage::models);
        timers::count_packed_material(64);
        timers::count_terrain_square();
        timers::count_atlas_emptied();
        timers::count_atlas_upload(10, true);
        timers::mark(Stage::match_setup);
        timers::end_marks();
        timers::note_scene({1, 2, 3, 4, 5, 6, 7, 8, 9}, 5);
    }
    for (std::size_t stage = 0; stage < timers::stage_count; ++stage) {
        OA_CHECK(timers::timers.totals.milliseconds[stage] == 0);
        OA_CHECK(timers::timers.totals.materials_packed[stage] == 0);
    }
    OA_CHECK(timers::timers.totals.terrain_squares_packed == 0);
    OA_CHECK(timers::timers.totals.atlas_emptied == 0);
    OA_CHECK(timers::timers.totals.atlas_rows_uploaded == 0);
    OA_CHECK(timers::timers.totals.ground_revisions == 0);
    OA_CHECK(timers::timers.last.vertices == 0);
    OA_CHECK(timers::timers.open == Stage::count);

    timers::reset(true);
    {
        timers::Span outer(Stage::scene_build);
        {
            const timers::Span inner(Stage::shadows);
            OA_CHECK(timers::timers.open == Stage::shadows);
            timers::count_packed_material(16 * 8);
            timers::count_packed_material(4);
        }
        OA_CHECK(timers::timers.open == Stage::scene_build);
        timers::count_packed_material(1);
        outer.stop();
        OA_CHECK(timers::timers.open == Stage::count);
        // Packing outside every span is charged to none.
        timers::count_packed_material(1000);
        outer.start();
        outer.stop();
    }
    const auto& totals = timers::timers.totals;
    const auto index = [](Stage stage) { return static_cast<std::size_t>(stage); };
    OA_CHECK(totals.materials_packed[index(Stage::shadows)] == 2);
    OA_CHECK(totals.texels_packed[index(Stage::shadows)] == 132);
    OA_CHECK(totals.materials_packed[index(Stage::scene_build)] == 1);
    OA_CHECK(totals.milliseconds[index(Stage::shadows)] >= 0);
    OA_CHECK(totals.milliseconds[index(Stage::scene_build)] >= 0);

    // The marked parts: each ends where the next begins.
    timers::mark(Stage::match_setup);
    timers::mark(Stage::match_hud);
    OA_CHECK(timers::timers.marked == Stage::match_hud);
    {
        const timers::MarkedParts parts;
    }
    OA_CHECK(timers::timers.marked == Stage::count);

    timers::count_terrain_square();
    timers::count_atlas_emptied();
    timers::count_atlas_upload(10, false);
    timers::count_atlas_upload(32, true);
    OA_CHECK(totals.terrain_squares_packed == 1 && totals.atlas_emptied == 1);
    OA_CHECK(totals.atlas_rows_uploaded == 42 && totals.whole_atlas_uploads == 1);

    // A frame counts as a ground revision when its revision differs from
    // the frame before's.
    timers::note_scene({100, 50, 3, 4, 5, 6, 7, 8, 0}, 7);
    timers::note_scene({100, 50, 3, 4, 5, 6, 7, 8, 0}, 7);
    timers::note_scene({100, 50, 3, 4, 5, 6, 7, 8, 0}, 8);
    timers::note_supersample(2);
    timers::note_multisample(4);
    OA_CHECK(totals.ground_revisions == 2);
    OA_CHECK(timers::timers.last.vertices == 100 && timers::timers.last.supersample == 2);
    OA_CHECK(timers::timers.multisample == 4);

    timers::reset(true);
    OA_CHECK(timers::timers.totals.ground_revisions == 0);
    OA_CHECK(timers::timers.totals.materials_packed[index(Stage::shadows)] == 0);
    OA_CHECK(timers::timers.last.vertices == 100 && timers::timers.ground_revision == 8);
    OA_CHECK(timers::timers.multisample == 4);
    timers::note_scene({100, 50, 3, 4, 5, 6, 7, 8, 0}, 8);
    OA_CHECK(timers::timers.totals.ground_revisions == 0);
    timers::reset(false);
    OA_CHECK(!timers::timers.on);
}

/// What the timers summed over a frame is the growth of their totals, and
/// the sums of several frames add up.
void totals_of_a_frame_add_up() {
    const auto index = [](Stage stage) { return static_cast<std::size_t>(stage); };
    timers::Totals before{};
    before.milliseconds[index(Stage::terrain)] = 2;
    before.materials_packed[index(Stage::models)] = 3;
    before.ground_revisions = 4;
    timers::Totals now = before;
    now.milliseconds[index(Stage::terrain)] = 7.5;
    now.texels_packed[index(Stage::models)] = 64;
    now.terrain_squares_packed = 12;
    now.atlas_emptied = 1;
    now.atlas_rows_uploaded = 600;
    now.whole_atlas_uploads = 1;
    now.ground_revisions = 5;
    const auto frame = timers::totals_since(now, before);
    OA_CHECK(near(frame.milliseconds[index(Stage::terrain)], 5.5));
    OA_CHECK(near(frame.materials_packed[index(Stage::models)], 0));
    OA_CHECK(near(frame.texels_packed[index(Stage::models)], 64));
    OA_CHECK(near(frame.terrain_squares_packed, 12) && near(frame.atlas_emptied, 1));
    OA_CHECK(near(frame.atlas_rows_uploaded, 600) && near(frame.whole_atlas_uploads, 1));
    OA_CHECK(near(frame.ground_revisions, 1));
    timers::Totals sum{};
    timers::add_totals(sum, frame);
    timers::add_totals(sum, frame);
    OA_CHECK(near(sum.milliseconds[index(Stage::terrain)], 11));
    OA_CHECK(near(sum.texels_packed[index(Stage::models)], 128));
    OA_CHECK(near(sum.terrain_squares_packed, 24) && near(sum.ground_revisions, 2));
    OA_CHECK(near(sum.atlas_rows_uploaded, 1200) && near(sum.whole_atlas_uploads, 2));
}

} // namespace

int main() {
    view_reads_every_key();
    rules_requests_preserve_defaults_and_require_enhanced_mode();
    started_rules_must_match_explicit_requests();
    view_refuses_invalid_rules_versions();
    gameplay_view_checks_only_the_starting_population();
    view_refuses_what_it_cannot_read();
    view_file_takes_its_folder();
    spread_takes_percentiles_and_the_slowest();
    slowest_frames_are_the_slowest_one_percent();
    lines_keep_their_layout();
    simulation_frames_separate_catchup_from_slow_steps();
    simulation_frames_preserve_tick_and_step_boundaries();
    simulation_frame_scopes_restore_their_recorders();
    timers_sum_only_while_on();
    totals_of_a_frame_add_up();
    if (oa::test::check_exit_status() != 0)
        return 1;
    std::puts(
        "bench: a view file's keys read, its errors name their line, its stage comes from its "
        "folder; the spreads, the lines and the timers, which sum nothing while off, hold"
    );
    return 0;
}
