// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --bench: a skirmish as the preferences set it up, a stage played into it,
// the camera held where a view file says, a warm-up to a match tick and a
// measured span on the application's own loop, its frames paced and
// presented as a player's are, reported in "bench:" lines
// (docs/command-line.md#--bench).
#include "oa/app/runtime.hpp"
#include "bench.hpp"
#include "bench_timers.hpp"
#include "oa/app/frame_coordinates.hpp"

#include "oa/core/unit.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

/// Nanoseconds in a second.
constexpr double kBenchNanosecondsPerSecond = 1e9;
/// Milliseconds in a second.
constexpr double kMillisecondsPerSecond = 1000.0;
/// Frames the measured span keeps room for before it grows.
constexpr std::size_t kExpectedFrames = 4096;
/// Players the unit counts tell apart: the local player, and the rest
/// together.
constexpr std::size_t kCountedPlayers = 2;

/// Throws the bench's error, which run_bench prints in its FAILED line.
///
/// @param what what went wrong
[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error(what);
}

/// The times one frame of the measured span took, in milliseconds.
struct FrameSample {
    double frame{};   ///< from the end of the frame before to the end of this one
    double work{};    ///< the loop's pass, without the wait for the frame's time
    double sim{};     ///< the match ticks the frame ran
    double draw{};    ///< composing the frame
    double present{}; ///< uploading and presenting it
};

/// Returns a measure's values, one a frame.
///
/// @param samples the frames
/// @param field the measure
/// @return its values
[[nodiscard]] std::vector<double>
measure_values(const std::vector<FrameSample>& samples, double FrameSample::* field) {
    std::vector<double> values;
    values.reserve(samples.size());
    for (const auto& sample : samples)
        values.push_back(sample.*field);
    return values;
}

} // namespace

void Runtime::run_bench() {
    // A bench nobody watches says why it stopped in its lines and ends with
    // status 1, rather than waiting on a message box.
    try {
        play_bench();
    } catch (const std::exception& error) {
        std::printf("bench: FAILED %s\n", error.what());
        std::fflush(stdout);
        exit_status_ = 1;
    }
}

bool Runtime::bench_ignores_event(const SDL_Event& event) const {
    if (options_.bench_file.empty())
        return false;
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_TEXT_EDITING:
    case SDL_EVENT_TEXT_INPUT:
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
    case SDL_EVENT_JOYSTICK_AXIS_MOTION:
    case SDL_EVENT_JOYSTICK_BALL_MOTION:
    case SDL_EVENT_JOYSTICK_HAT_MOTION:
    case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
    case SDL_EVENT_JOYSTICK_BUTTON_UP:
    case SDL_EVENT_PEN_DOWN:
    case SDL_EVENT_PEN_UP:
    case SDL_EVENT_PEN_MOTION:
    case SDL_EVENT_PEN_BUTTON_DOWN:
    case SDL_EVENT_PEN_BUTTON_UP:
    case SDL_EVENT_PEN_AXIS:
        return true;
    default:
        return false;
    }
}

void Runtime::play_bench() {
    if (sdl_.window == nullptr || sdl_.renderer == nullptr)
        fail("needs the game's window");
    bench::View view;
    if (std::string error; !bench::read_view(options_.bench_file, view, error))
        fail(error);
    const char* label = view.label.c_str();
    bool running = true;
    const auto now = [] {
        return static_cast<double>(SDL_GetTicksNS()) / kBenchNanosecondsPerSecond;
    };
    // One pass of the application loop, paced as run() paces it; the time it
    // worked, without the wait, is kept in `work`.
    double work = 0.0;
    const auto frame = [&] {
        const double start = now();
        run_frame(running);
        work = now() - start;
        if (!running || exit_requested_)
            fail("the game was closed");
        pace_next_frame(running);
    };
    const double menu_end = now() + view.menu_seconds;
    while (now() < menu_end)
        frame();
    exercise_click(menu::resource_name(menu::Button::single_player));
    exercise_click(entry::resource_name(entry::Button::skirmish));
    state_.player_count = view.players;
    exercise_click(skirmish::resource_name(skirmish::Button::start));
    if (screen_ != Screen::match || !match_)
        fail("the skirmish did not start");
    if (!view.stage.empty()) {
        options_.stage_file = view.stage;
        apply_stage();
    }
    show_frame_stats(true);
    // The warm-up's frames are timed too, so that the kept ground's
    // revision the span starts from is the frame before's.
    bench_timers::reset(view.stage_timers);
    double applied_yaw = std::numeric_limits<double>::quiet_NaN();
    float applied_zoom = -1;
    // Holds the view at the file's zoom and centre, and turns the camera to
    // a yaw at the file's pitch; yaw and pitch 0 keep the classic view.
    const auto aim = [&](double yaw) {
        const float zoom =
            view.zoom <= 0
                ? least_match_zoom()
                : std::clamp(static_cast<float>(view.zoom), least_match_zoom(), most_match_zoom());
        if (zoom != applied_zoom) {
            match_zoom_ = match_zoom_target_ = zoom;
            place_match_view(
                view.centre_x - match_layout_.battlefield_width() / (2.0 * zoom),
                view.centre_z - match_layout_.battlefield_height() / (2.0 * zoom)
            );
            applied_zoom = zoom;
        }
        const bool turned = yaw != 0 || view.pitch != 0;
        if (turned && (yaw != applied_yaw || !live_renderer_active())) {
            turn_live_camera(static_cast<float>(yaw), static_cast<float>(view.pitch));
            applied_yaw = yaw;
        }
    };
    // The pointer rests over the middle of the battlefield, as a player's
    // does, so that the frame's pick of the unit under it is a player's.
    const auto point_at_battle = [&] {
        float x{}, y{};
        if (!frame_to_window(
                sdl_.renderer,
                static_cast<float>(match_layout_.left + match_layout_.battlefield_width() / 2),
                static_cast<float>(match_layout_.top + match_layout_.battlefield_height() / 2),
                &x,
                &y
            ))
            return;
        SDL_Event event{};
        event.type = SDL_EVENT_MOUSE_MOTION;
        event.motion.windowID = SDL_GetWindowID(sdl_.window);
        event.motion.x = x;
        event.motion.y = y;
        handle_sdl_event(event, running);
    };
    std::printf("bench: WARMUP label=%s to tick %u\n", label, view.warmup_ticks);
    std::fflush(stdout);
    aim(view.yaw);
    point_at_battle();
    while (match_timing_.tick < view.warmup_ticks) {
        aim(view.yaw);
        frame();
        if (screen_ != Screen::match)
            fail("the match ended in the warm-up");
    }
    point_at_battle();
    // The live units of the local player and of the others.
    const auto count_units = [&] {
        std::array<int, kCountedPlayers> units{};
        for (const auto& slot : match_->world().slots)
            if (slot.unit != nullptr && slot.record.type_index != 0 &&
                oa::unit_is_live_target(slot.record.flags))
                ++units[slot.record.owner_index == match_local_player_ ? 0 : 1];
        return units;
    };
    std::vector<FrameSample> samples;
    samples.reserve(kExpectedFrames);
    bench_timers::reset(view.stage_timers);
    const uint32_t first_tick = match_timing_.tick;
    std::printf(
        "bench: MEASURING label=%s tick=%u seconds=%.0f\n", label, first_tick, view.measure_seconds
    );
    std::fflush(stdout);
    const double start = now();
    double previous = start;
    bool shot = false;
    int second = 0;
    // Frames drawn while the window was away, which another window may have
    // covered or the system may have slowed.
    std::size_t away_frames = 0;
    std::size_t second_first = 0;
    for (;;) {
        const double elapsed = now() - start;
        if (elapsed >= view.measure_seconds ||
            (view.measure_ticks != 0 && match_timing_.tick - first_tick >= view.measure_ticks))
            break;
        aim(view.yaw + view.turn_rate * elapsed);
        const auto phases = phase_times_;
        frame();
        const double finished = now();
        const auto ms = [](int64_t ns) {
            return static_cast<double>(ns) * kMillisecondsPerSecond / kBenchNanosecondsPerSecond;
        };
        samples.push_back(
            {(finished - previous) * kMillisecondsPerSecond,
             work * kMillisecondsPerSecond,
             ms(phase_times_.simulation - phases.simulation),
             ms(phase_times_.compose - phases.compose),
             ms((phase_times_.upload - phases.upload) + (phase_times_.present - phases.present))}
        );
        previous = finished;
        if (window_away())
            ++away_frames;
        const uint32_t span_ticks = match_timing_.tick - first_tick;
        if (!shot && (view.shot_ticks != 0 ? span_ticks >= view.shot_ticks
                                           : finished - start >= view.shot_at)) {
            std::printf("bench: SHOT label=%s\n", label);
            std::fflush(stdout);
            shot = true;
        }
        if (finished - start >= second + 1) {
            double frame_total = 0, sim_total = 0, draw_total = 0;
            const std::size_t frames = samples.size() - second_first;
            for (std::size_t index = second_first; index < samples.size(); ++index) {
                frame_total += samples[index].frame;
                sim_total += samples[index].sim;
                draw_total += samples[index].draw;
            }
            const auto mean = [&](double total) {
                return frames == 0 ? 0.0 : total / static_cast<double>(frames);
            };
            const auto units = count_units();
            std::printf(
                "bench: SEC %d frames %zu frame %.1f sim %.1f draw %.1f units %d+%d vertices %.0f "
                "tick %u\n",
                second,
                frames,
                mean(frame_total),
                mean(sim_total),
                mean(draw_total),
                units[0],
                units[1],
                bench_timers::timers.last.vertices,
                match_timing_.tick
            );
            std::fflush(stdout);
            ++second;
            second_first = samples.size();
        }
        if (screen_ != Screen::match)
            break;
    }
    const double seconds = previous - start;
    const std::size_t frames = samples.size();
    const uint32_t ticks = match_timing_.tick - first_tick;
    const auto frame_spread = bench::spread(measure_values(samples, &FrameSample::frame));
    const auto per_frame = [&](double total) {
        return frames == 0 ? 0.0 : total / static_cast<double>(frames);
    };
    std::printf(
        "bench: RESULT label=%s seconds %.2f frames %zu fps %.2f low1_fps %.2f ticks %u (%.1f/s) "
        "game speed %d of %d\n",
        label,
        seconds,
        frames,
        seconds > 0 ? static_cast<double>(frames) / seconds : 0.0,
        bench::frames_per_second(frame_spread.low1),
        ticks,
        seconds > 0 ? ticks / seconds : 0.0,
        static_cast<int>(match_timing_.actual_rate),
        static_cast<int>(match_timing_.requested_rate)
    );
    if (away_frames != 0)
        std::printf(
            "bench: AWAY label=%s frames %zu of %zu: the window was away, so the span may not "
            "measure what a player sees\n",
            label,
            away_frames,
            frames
        );
    double sim_total = 0;
    for (const auto& sample : samples)
        sim_total += sample.sim;
    const std::array<std::pair<const char*, double FrameSample::*>, 5> measures{
        {{"frame", &FrameSample::frame},
         {"work", &FrameSample::work},
         {"sim(per frame)", &FrameSample::sim},
         {"draw", &FrameSample::draw},
         {"present", &FrameSample::present}}
    };
    for (const auto& [name, field] : measures)
        std::printf(
            "%s\n", bench::measure_line(name, bench::spread(measure_values(samples, field))).c_str()
        );
    std::printf("bench: MEASURE tick(per tick)    avg %8.2f\n", ticks ? sim_total / ticks : 0.0);
    const auto& totals = bench_timers::timers.totals;
    const auto& scene = bench_timers::timers.last;
    if (view.stage_timers)
        for (std::size_t stage = 0; stage < bench_timers::stage_count; ++stage)
            std::printf(
                "%s\n",
                bench::stage_line(
                    static_cast<bench_timers::Stage>(stage), totals.milliseconds[stage], frames
                )
                    .c_str()
            );
    const auto units = count_units();
    std::printf(
        "bench: COUNTS units %d (player0 %d, others %d) drawn %u vertices %.0f terrain %.0f "
        "models %.0f sprites %.0f particles %.0f nano %.0f shadows %.0f crop_cells %.0f "
        "projectiles %zu tick %u supersample %.0f multisample %.0f battlefield %dx%d "
        "atlas_rows %.0f full_atlas_uploads %.0f\n",
        units[0] + units[1],
        units[0],
        units[1],
        frame_draws_.units_drawn,
        scene.vertices,
        scene.terrain_vertices,
        scene.models,
        scene.sprites,
        scene.particles,
        scene.nano_particles,
        scene.shadows,
        scene.crop_cells,
        match_->projectiles().size(),
        match_timing_.tick,
        scene.supersample,
        bench_timers::timers.multisample,
        match_layout_.battlefield_width(),
        match_layout_.battlefield_height(),
        per_frame(totals.atlas_rows_uploaded),
        totals.whole_atlas_uploads
    );
    std::printf("bench: GROUND revisions %.0f in %zu frames\n", totals.ground_revisions, frames);
    std::printf(
        "bench: ATLAS terrain_packs %.1f a frame, atlas_resets %.0f in %zu frames\n",
        per_frame(totals.terrain_squares_packed),
        totals.atlas_emptied,
        frames
    );
    for (std::size_t stage = 0; stage < bench_timers::stage_count; ++stage)
        if (totals.materials_packed[stage] > 0) {
            const auto& name = bench_timers::stage_names[stage];
            const std::string shown = (name.nested ? "  " : "") + std::string(name.name);
            std::printf(
                "bench: ATLAS packed by %-34s %8.1f materials %10.0f texels a frame\n",
                shown.c_str(),
                per_frame(totals.materials_packed[stage]),
                per_frame(totals.texels_packed[stage])
            );
        }
    std::fflush(stdout);
    bench_timers::reset(false);
    const double hold_end = now() + view.hold_after;
    while (now() < hold_end)
        frame();
    std::printf("bench: DONE label=%s\n", label);
    std::fflush(stdout);
}

} // namespace oa::app
