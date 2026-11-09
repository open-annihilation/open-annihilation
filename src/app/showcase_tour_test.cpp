// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Showcase camera files, transactional refusals, timed cuts and eased movement.
#include "showcase_tour.hpp"
#include "oa/test/check.hpp"

#include <limits>

namespace {

using namespace oa::app;

/// Compares all camera values exactly, including a stored endpoint's zoom.
bool same_view(const StageView& first, const StageView& second) {
    return first.centre_x == second.centre_x && first.centre_z == second.centre_z &&
           first.zoom == second.zoom && first.yaw == second.yaw && first.pitch == second.pitch;
}

/// Allows the rounding of logarithmic zoom interpolation.
bool near(double first, double second) {
    return std::abs(first - second) < 1e-10;
}

/// Reads comments, complete camera values and each transition kind.
void complete_camera_file() {
    std::vector<ShowcaseKeyframe> frames;
    std::string error = "old error";
    OA_CHECK(parse_showcase_tour(
        " # camera tour\r\n\n0 cut 5120 5120 .25 20 -53\n"
        "60 move 5120 7360 .5 20 -32\n90 cut 8000 1888 .8 0 -20  \n",
        90,
        frames,
        error
    ));
    OA_CHECK(error.empty());
    OA_CHECK(frames.size() == 3);
    if (frames.size() != 3)
        return;
    OA_CHECK(frames[0].seconds == 0 && frames[0].transition == ShowcaseTransition::cut);
    OA_CHECK(frames[1].seconds == 60 && frames[1].transition == ShowcaseTransition::move);
    OA_CHECK(frames[2].seconds == 90 && frames[2].transition == ShowcaseTransition::cut);
    OA_CHECK(same_view(frames[0].view, {5120, 5120, .25, 20, -53}));
    OA_CHECK(same_view(frames[2].view, {8000, 1888, .8, 0, -20}));
}

/// Refuses malformed lines and durations without replacing an earlier tour.
void malformed_camera_files_preserve_output() {
    const std::vector<ShowcaseKeyframe> sentinel{{17, ShowcaseTransition::cut, {1, 2, .5, 4, 5}}};
    std::vector<ShowcaseKeyframe> frames = sentinel;
    std::string error;
    for (const auto* text :
         {"",
          " \n# no views\n",
          "1 cut 1 2 .5 4 5",
          "-1 cut 1 2 .5 4 5",
          "0 cut 1 2 .5 4 5\n0 move 2 3 .75 4 5",
          "0 cut 1 2 .5 4 5\n2 move 2 3 .75 4 5\n1 move 3 4 1 4 5",
          "0 cut 1 2 .5 4 5\n11 move 2 3 .75 4 5",
          "0 slide 1 2 .5 4 5",
          "0 cut 1 2 .5 4",
          "0 cut 1 2 .5 4 5 extra",
          "0 cut 1 2 0 4 5",
          "0 cut 1 2 -1 4 5",
          "0 cut 1 2 nan 4 5",
          "0 cut 1 2 .5 inf 5",
          "nan cut 1 2 .5 4 5",
          "inf cut 1 2 .5 4 5",
          "0 cut 1 2 .5 4 1e300",
          "0 cut 1 2 .5 4 5\nno time"}) {
        OA_CHECK(!parse_showcase_tour(text, 10, frames, error));
        OA_CHECK(!error.empty());
        OA_CHECK(frames.size() == sentinel.size());
        if (frames.size() != sentinel.size())
            continue;
        OA_CHECK(frames[0].seconds == sentinel[0].seconds);
        OA_CHECK(frames[0].transition == sentinel[0].transition);
        OA_CHECK(same_view(frames[0].view, sentinel[0].view));
    }
    OA_CHECK(error.starts_with("line 2:"));
    for (const double duration :
         {0.,
          -1.,
          std::numeric_limits<double>::infinity(),
          std::numeric_limits<double>::quiet_NaN()}) {
        OA_CHECK(!parse_showcase_tour("0 cut 1 2 .5 4 5", duration, frames, error));
        OA_CHECK(frames.size() == 1 && frames[0].seconds == 17);
    }
}

/// Accepts each file limit exactly and refuses the first entry beyond it.
void file_limits() {
    std::vector<ShowcaseKeyframe> frames;
    std::string error;
    std::string text = "0 cut 1 2 .5 4 5\n#";
    text.resize(max_showcase_tour_bytes, ' ');
    OA_CHECK(parse_showcase_tour(text, 512, frames, error));
    text.push_back(' ');
    OA_CHECK(!parse_showcase_tour(text, 512, frames, error));
    OA_CHECK(frames.size() == 1);
    text.clear();
    for (std::size_t number = 0; number < max_showcase_tour_keyframes; ++number)
        text += std::to_string(number) + " move 1 2 .5 4 5\n";
    OA_CHECK(parse_showcase_tour(text, 512, frames, error));
    OA_CHECK(frames.size() == max_showcase_tour_keyframes);
    text += "512 cut 1 2 .5 4 5\n";
    OA_CHECK(!parse_showcase_tour(text, 512, frames, error));
    OA_CHECK(frames.size() == max_showcase_tour_keyframes);
}

/// Eases movement, scales zoom proportionally and crosses the yaw wrap briefly.
void eased_camera_movement() {
    const std::vector<ShowcaseKeyframe> frames{
        {0, ShowcaseTransition::cut, {0, 100, .25, 170, -50}},
        {8, ShowcaseTransition::move, {80, 20, 1, -170, -10}},
        {12, ShowcaseTransition::move, {80, 20, 1, -170, -10}}
    };
    const auto middle = sample_showcase_tour(frames, 4);
    OA_CHECK(middle.has_value());
    if (!middle)
        return;
    OA_CHECK(middle->centre_x == 40 && middle->centre_z == 60);
    OA_CHECK(near(middle->zoom, .5) && middle->yaw == 180 && middle->pitch == -30);
    const auto quarter = sample_showcase_tour(frames, 2);
    OA_CHECK(quarter.has_value());
    if (!quarter)
        return;
    // Smoothstep at one quarter is 5/32; the zoom's ratio is 4^(5/32).
    OA_CHECK(quarter->centre_x == 12.5 && quarter->centre_z == 87.5);
    OA_CHECK(quarter->pitch == -43.75 && quarter->yaw == 173.125);
    OA_CHECK(near(quarter->zoom, .25 * std::pow(4., 5. / 32.)));
    for (const double elapsed : {-10., 0., 8., 10., 12., 99.}) {
        const auto sampled = sample_showcase_tour(frames, elapsed);
        OA_CHECK(sampled.has_value());
        if (sampled)
            OA_CHECK(same_view(*sampled, elapsed <= 0 ? frames.front().view : frames.back().view));
    }
}

/// Holds views until a cut, holds single views and leaves absent tours unset.
void cuts_single_views_and_absent_tours() {
    const std::vector<ShowcaseKeyframe> frames{
        {0, ShowcaseTransition::move, {1, 2, .5, 4, 5}},
        {5, ShowcaseTransition::cut, {11, 12, 2, 14, 15}},
        {10, ShowcaseTransition::move, {21, 22, 8, 24, 25}}
    };
    for (const double elapsed : {0., 2.5, 4.999}) {
        const auto sampled = sample_showcase_tour(frames, elapsed);
        OA_CHECK(sampled && same_view(*sampled, frames[0].view));
    }
    const auto at_cut = sample_showcase_tour(frames, 5);
    OA_CHECK(at_cut && same_view(*at_cut, frames[1].view));
    const auto after_cut = sample_showcase_tour(frames, 7.5);
    OA_CHECK(after_cut.has_value());
    if (after_cut) {
        OA_CHECK(after_cut->centre_x == 16 && after_cut->centre_z == 17);
        OA_CHECK(near(after_cut->zoom, 4) && after_cut->yaw == 19 && after_cut->pitch == 20);
    }
    const std::span<const ShowcaseKeyframe> single(frames.data(), 1);
    for (const double elapsed : {-1., 0., 100.}) {
        const auto sampled = sample_showcase_tour(single, elapsed);
        OA_CHECK(sampled && same_view(*sampled, frames[0].view));
    }
    OA_CHECK(!sample_showcase_tour({}, 0));
    OA_CHECK(!sample_showcase_tour(frames, std::numeric_limits<double>::quiet_NaN()));
    OA_CHECK(!sample_showcase_tour(frames, std::numeric_limits<double>::infinity()));
}

/// Keeps interpolated camera values finite across the accepted numeric range.
void finite_camera_extremes() {
    std::vector<ShowcaseKeyframe> frames;
    std::string error;
    OA_CHECK(parse_showcase_tour(
        "0 cut -3e38 3e38 1e-300 3e38 -3e38\n"
        "2 move 3e38 -3e38 3e38 -3e38 3e38",
        2,
        frames,
        error
    ));
    if (frames.size() != 2)
        return;
    for (const double elapsed : {.001, .5, 1., 1.5, 1.999}) {
        const auto sampled = sample_showcase_tour(frames, elapsed);
        OA_CHECK(sampled.has_value());
        if (!sampled)
            continue;
        OA_CHECK(std::isfinite(sampled->centre_x) && std::isfinite(sampled->centre_z));
        OA_CHECK(std::isfinite(sampled->yaw) && std::isfinite(sampled->pitch));
        OA_CHECK(std::isfinite(sampled->zoom) && sampled->zoom >= 1e-300 && sampled->zoom <= 3e38);
    }
}

} // namespace

int main() {
    complete_camera_file();
    malformed_camera_files_preserve_output();
    file_limits();
    eased_camera_movement();
    cuts_single_views_and_absent_tours();
    finite_camera_extremes();
    return oa::test::check_exit_status();
}
