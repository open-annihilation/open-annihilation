// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A showcase's camera tour, sampled in elapsed presentation seconds.
#pragma once

#include "stage_view.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {

/// How the camera reaches the view named by a keyframe.
enum class ShowcaseTransition { move, cut };

/// One camera view at a time measured from the showcase's start.
struct ShowcaseKeyframe {
    double seconds{};                ///< elapsed presentation seconds
    ShowcaseTransition transition{}; ///< move from the preceding view, or cut at this time
    StageView view{};                ///< the camera at this time
};

/// The most text a showcase camera tour accepts, in bytes.
inline constexpr std::size_t max_showcase_tour_bytes = 65'536;
/// The most camera views a showcase tour accepts.
inline constexpr std::size_t max_showcase_tour_keyframes = 512;

/// Reads one "SECONDS cut|move X Z ZOOM YAW PITCH" camera view per line.
///
/// Blank lines and lines whose first nonspace character is # are skipped.
/// Times must start at zero, strictly increase, and fit within the positive
/// finite duration. Camera values follow read_stage_view's bounds. The output
/// is replaced only when the entire tour is valid, and error is cleared then.
///
/// @param text the camera tour's text
/// @param duration the showcase's duration, in presentation seconds
/// @param[out] out the parsed camera views, unchanged on failure
/// @param[out] error why the tour was refused, with a line number for a bad line
/// @return true when the complete tour was read
inline bool parse_showcase_tour(
    std::string_view text, double duration, std::vector<ShowcaseKeyframe>& out, std::string& error
) {
    if (!std::isfinite(duration) || duration <= 0) {
        error = "the showcase duration must be finite and positive";
        return false;
    }
    if (text.size() > max_showcase_tour_bytes) {
        error = "the showcase tour exceeds its text limit";
        return false;
    }
    std::vector<ShowcaseKeyframe> parsed;
    std::istringstream input{std::string(text)};
    std::string text_line;
    for (std::size_t number = 1; std::getline(input, text_line); ++number) {
        std::istringstream line(text_line);
        line >> std::ws;
        if (line.eof() || line.peek() == '#')
            continue;
        const auto refuse = [&](std::string_view reason) {
            error = "line " + std::to_string(number) + ": " + std::string(reason);
            return false;
        };
        if (parsed.size() == max_showcase_tour_keyframes)
            return refuse("the showcase tour exceeds its keyframe limit");
        ShowcaseKeyframe frame;
        std::string transition;
        if (!(line >> frame.seconds >> transition) || !std::isfinite(frame.seconds))
            return refuse("a camera view needs finite seconds and a cut or move transition");
        if (frame.seconds < 0 || frame.seconds > duration)
            return refuse("camera view seconds must be within the showcase duration");
        if (parsed.empty() ? frame.seconds != 0 : frame.seconds <= parsed.back().seconds)
            return refuse("camera view seconds must start at zero and strictly increase");
        if (transition == "cut")
            frame.transition = ShowcaseTransition::cut;
        else if (transition == "move")
            frame.transition = ShowcaseTransition::move;
        else
            return refuse("a camera transition must be cut or move");
        if (!read_stage_view(line, frame.view))
            return refuse(
                "a camera view needs X Z ZOOM YAW PITCH with finite values and positive zoom"
            );
        parsed.push_back(frame);
    }
    if (parsed.empty()) {
        error = "the showcase tour needs at least one camera view";
        return false;
    }
    out = std::move(parsed);
    error.clear();
    return true;
}

/// Samples a parsed camera tour without reading or changing the match.
///
/// Before the first view and after the last, that endpoint is held exactly.
/// A cut holds the preceding view until its keyframe's time, then jumps. A
/// move eases its fraction u with 3*u*u - 2*u*u*u, interpolates centre and
/// pitch linearly, zoom logarithmically, and yaw along the shortest wrapped
/// turn. Intermediate yaw may differ from a stored yaw by whole turns.
///
/// @param frames the views of a successfully parsed tour
/// @param elapsed elapsed presentation seconds
/// @return the camera, or no view for an empty tour or nonfinite elapsed time
inline std::optional<StageView>
sample_showcase_tour(std::span<const ShowcaseKeyframe> frames, double elapsed) {
    if (frames.empty() || !std::isfinite(elapsed))
        return std::nullopt;
    if (elapsed <= frames.front().seconds)
        return frames.front().view;
    if (elapsed >= frames.back().seconds)
        return frames.back().view;
    const auto next = std::upper_bound(
        frames.begin(), frames.end(), elapsed, [](double seconds, const ShowcaseKeyframe& frame) {
            return seconds < frame.seconds;
        }
    );
    const auto& previous = *(next - 1);
    if (elapsed == previous.seconds || next->transition == ShowcaseTransition::cut)
        return previous.view;
    const double u = (elapsed - previous.seconds) / (next->seconds - previous.seconds);
    const double eased = u * u * (3 - 2 * u);
    const auto& from = previous.view;
    const auto& to = next->view;
    constexpr double full_turn = 360;
    const double yaw = std::remainder(from.yaw, full_turn);
    const double turn = std::remainder(std::remainder(to.yaw, full_turn) - yaw, full_turn);
    const double zoom = std::exp(std::lerp(std::log(from.zoom), std::log(to.zoom), eased));
    return StageView{
        std::lerp(from.centre_x, to.centre_x, eased),
        std::lerp(from.centre_z, to.centre_z, eased),
        std::clamp(zoom, std::min(from.zoom, to.zoom), std::max(from.zoom, to.zoom)),
        yaw + turn * eased,
        std::lerp(from.pitch, to.pitch, eased)
    };
}

} // namespace oa::app
