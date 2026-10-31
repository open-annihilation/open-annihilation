// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The parts of --bench that hold no game: a view file's keys and defaults,
// the keys and values it refuses, a stage taken from the file's folder and
// the label from its name; the spread of a measure, its percentiles and its
// slowest 1%; the MEASURE and STAGE lines; and the stage timers, which sum
// nothing while off, and time stages, parts and the atlas's counts while on.
#include "bench.hpp"
#include "bench_timers.hpp"

#include "oa/platform/files.hpp"
#include "oa/test/check.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
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

    bench::View view;
    const char* text = "# the low view\n"
                       "label low-across\r\n"
                       "stage battle.stage\n"
                       "players 3\n"
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
                       "stage_timers 0";
    OA_CHECK(bench::parse_view(text, view, error));
    OA_CHECK(view.label == "low-across");
    OA_CHECK(oa::platform::path_to_generic_utf8(view.stage) == "battle.stage");
    OA_CHECK(view.players == 3 && near(view.menu_seconds, 0.5));
    OA_CHECK(view.warmup_ticks == 60 && view.measure_ticks == 750 && view.shot_ticks == 360);
    OA_CHECK(near(view.measure_seconds, 150) && near(view.shot_at, 12));
    OA_CHECK(near(view.centre_x, 1980) && near(view.centre_z, 2080));
    OA_CHECK(near(view.yaw, 20) && near(view.pitch, -53) && near(view.zoom, 0));
    OA_CHECK(near(view.turn_rate, 6) && near(view.hold_after, 2) && !view.stage_timers);
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
    // Every stage has a name, and the names are told apart.
    for (std::size_t first = 0; first < timers::stage_count; ++first) {
        OA_CHECK(!timers::stage_names[first].name.empty());
        for (std::size_t second = first + 1; second < timers::stage_count; ++second)
            OA_CHECK(timers::stage_names[first].name != timers::stage_names[second].name);
    }
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

} // namespace

int main() {
    view_reads_every_key();
    view_refuses_what_it_cannot_read();
    view_file_takes_its_folder();
    spread_takes_percentiles_and_the_slowest();
    lines_keep_their_layout();
    timers_sum_only_while_on();
    if (oa::test::check_exit_status() != 0)
        return 1;
    std::puts(
        "bench: a view file's keys read, its errors name their line, its stage comes from its "
        "folder; the spreads, the lines and the timers, which sum nothing while off, hold"
    );
    return 0;
}
