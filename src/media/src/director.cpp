// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The director's camera: views kept on the map, the engine camera that draws
// a view, a script's shots compiled to frames, and the rig that steps them
// (director.hpp). Every view is computed with the basic operations of
// doubles, conversions and floor, in the order director.hpp writes them.

#include "oa/media/director.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace oa::media::director {
namespace {

using oa::formats::oascript::CameraState;
using oa::formats::oascript::Decimal;
using oa::formats::oascript::Motion;
using oa::formats::oascript::Script;
using oa::formats::oascript::Shot;

/// The base of a decimal's places.
inline constexpr double decimal_base = 10.0;

/// Returns a decimal as a double: its mantissa divided by 10^places.
///
/// 10^places is formed by multiplying by 10, exact for every place count a
/// script allows.
///
/// @param value the decimal
/// @return the double
double decimal_value(Decimal value) noexcept {
    double scale{1.0};
    for (uint32_t place{}; place < value.places; ++place)
        scale = scale * decimal_base;
    return static_cast<double>(value.mantissa) / scale;
}

/// Returns a value held within [low, high]; not-a-number gives low.
///
/// @param value the value
/// @param low the smallest result
/// @param high the largest result, at least low
/// @return the held value
double held_within(double value, double low, double high) noexcept {
    if (!(value >= low))
        return low;
    return value > high ? high : value;
}

/// Converts a double to int32, rounding toward zero and holding it within
/// the type; not-a-number gives 0.
///
/// @param value a whole number, usually
/// @return the converted value
int32_t to_int32(double value) noexcept {
    constexpr double low{static_cast<double>(std::numeric_limits<int32_t>::min())};
    constexpr double high{static_cast<double>(std::numeric_limits<int32_t>::max())};
    if (!(value >= low))
        return value > 0.0 ? std::numeric_limits<int32_t>::max()
                           : (value < 0.0 ? std::numeric_limits<int32_t>::min() : 0);
    return value > high ? std::numeric_limits<int32_t>::max() : static_cast<int32_t>(value);
}

/// Converts a double to uint32, rounding toward zero and holding it within
/// the type; not-a-number and values below zero give 0.
///
/// @param value a whole number, usually
/// @return the converted value
uint32_t to_uint32(double value) noexcept {
    constexpr double high{static_cast<double>(std::numeric_limits<uint32_t>::max())};
    if (!(value >= 0.0))
        return 0;
    return value > high ? std::numeric_limits<uint32_t>::max() : static_cast<uint32_t>(value);
}

/// Returns a clock's frame rate as a double: numerator / denominator.
///
/// @param clock the clock
/// @return frames a second
double frames_per_second(const FrameClock& clock) noexcept {
    return static_cast<double>(clock.framerate.numerator) /
           static_cast<double>(clock.framerate.denominator);
}

/// Returns a clock's frame duration as a double: denominator / numerator.
///
/// @param clock the clock
/// @return seconds a frame
double frame_seconds(const FrameClock& clock) noexcept {
    return static_cast<double>(clock.framerate.denominator) /
           static_cast<double>(clock.framerate.numerator);
}

/// Returns a linear motion's value at frame k of n.
///
/// @param start the value at frame 0
/// @param end the value at frame n
/// @param frame k
/// @param frames n, at least 1
/// @return start + (end - start) * (k / n)
double linear_value(double start, double end, uint64_t frame, uint64_t frames) noexcept {
    return start + (end - start) * (static_cast<double>(frame) / static_cast<double>(frames));
}

/// Returns a linear motion's constant speed.
///
/// @param start the value at frame 0
/// @param end the value at frame n
/// @param fps frames a second
/// @param frames n, at least 1
/// @return (end - start) * fps / n, per second
double linear_speed(double start, double end, double fps, uint64_t frames) noexcept {
    return (end - start) * fps / static_cast<double>(frames);
}

/// One axis's spring constants for a shot.
struct Spring {
    double end{};          ///< the value it pulls toward
    double stiffness{};    ///< w * w, w = 2 pi frequency
    double damping{};      ///< 2 d w
    double frame_length{}; ///< dt, seconds a frame
};

/// Returns a shot's spring constants for one axis.
///
/// @param shot the shot
/// @param end the axis's end value
/// @param frame_length seconds a frame
/// @return the constants
Spring spring_of(const CompiledShot& shot, double end, double frame_length) noexcept {
    const double angular{2.0 * pi * shot.frequency};
    return Spring{end, angular * angular, 2.0 * shot.damping_ratio * angular, frame_length};
}

/// Steps one axis of a spring by one frame: first its speed, then its value.
///
/// @param spring the constants
/// @param[in,out] value the axis's value
/// @param[in,out] speed its speed, per second
void step_spring(const Spring& spring, double& value, double& speed) noexcept {
    speed = speed + spring.frame_length *
                        (spring.stiffness * (spring.end - value) - spring.damping * speed);
    value = value + spring.frame_length * speed;
}

/// Sets one axis at the start of a shot.
///
/// A shot with a start of its own starts there at rest; a continuing shot
/// keeps the value and speed the shot before reached. A linear shot then
/// moves at its own constant speed.
///
/// @param shot the shot
/// @param start the axis's start, when the shot has one
/// @param end the axis's end
/// @param fps frames a second
/// @param[in,out] value the axis's value
/// @param[in,out] speed its speed, per second
void begin_axis(
    const CompiledShot& shot, double start, double end, double fps, double& value, double& speed
) noexcept {
    if (!shot.continues) {
        value = start;
        speed = 0.0;
    }
    if (shot.motion == Motion::linear)
        speed = linear_speed(value, end, fps, shot.frame_count);
}

/// Moves one axis to its state at the end of a shot, frame n.
///
/// A spring has already been stepped there; a linear axis, which holds its
/// start while the shot runs, is set to its value at frame n.
///
/// @param shot the shot
/// @param end the axis's end
/// @param[in,out] value the axis's value
void end_axis(const CompiledShot& shot, double end, double& value) noexcept {
    if (shot.motion == Motion::linear)
        value = linear_value(value, end, shot.frame_count, shot.frame_count);
}

/// Returns a shot's key path in the script.
///
/// @param index the shot's index in director.shots
/// @return director.shots[index]
std::string shot_path(size_t index) {
    return "director.shots[" + std::to_string(index) + "]";
}

/// Adds the warnings for a camera that does not fit the map.
///
/// @param camera the camera
/// @param path its key path
/// @param bounds the map's bounds
/// @param output the output size
/// @param[in,out] messages the messages to add to
void warn_about_camera(
    const CameraState& camera,
    const std::string& path,
    MapBounds bounds,
    OutputSize output,
    CompileMessages& messages
) {
    const View view{view_of_camera(camera)};
    const View kept{clamp_view(view, bounds, output)};
    if (view.height < min_view_height)
        messages.warnings.push_back(
            path + ".position.y: the view height is below the smallest, " +
            std::to_string(static_cast<int32_t>(min_view_height)) +
            " map pixels; it is raised to that"
        );
    else if (view.height > fit_height(bounds, output))
        messages.warnings.push_back(
            path + ".position.y: the view height is above the height at which the whole map "
                   "fits the output; it is lowered to that"
        );
    if (kept.x != view.x || kept.z != view.z)
        messages.warnings.push_back(
            path + ".position: the view reaches past the map's edge; its centre is moved to keep "
                   "it on the map"
        );
    if (kept.pitch != view.pitch)
        messages.warnings.push_back(
            path + ".orientation.pitch: the camera would look down at an elevation outside " +
            std::to_string(static_cast<int32_t>(min_elevation)) + " to " +
            std::to_string(static_cast<int32_t>(max_elevation)) + " degrees; it is held within them"
        );
}

/// Compiles one shot's cameras, motion and transition.
///
/// @param shots the list being compiled; its clock is set
/// @param source the shot in the script
/// @param index its index in director.shots
/// @param[out] shot the compiled shot's cameras, motion and transition
/// @param[in,out] messages the messages to add to
/// @return true when there was no error
bool compile_shot_motion(
    const ShotList& shots,
    const Shot& source,
    size_t index,
    CompiledShot& shot,
    CompileMessages& messages
) {
    bool ok{true};
    const std::string path{shot_path(index)};
    shot.tick = source.tick;
    shot.script_index = index;
    shot.continues = !source.camera_start.has_value();
    if (source.camera_start) {
        shot.start = view_of_camera(*source.camera_start);
        warn_about_camera(
            *source.camera_start, path + ".cameraStart", shots.bounds, shots.output, messages
        );
    } else if (index == 0) {
        messages.errors.push_back(path + ".cameraStart: the first shot needs a starting camera");
        ok = false;
    }
    shot.end = view_of_camera(source.camera_end);
    warn_about_camera(source.camera_end, path + ".cameraEnd", shots.bounds, shots.output, messages);

    shot.motion = source.motion.kind;
    shot.frequency = decimal_value(source.motion.frequency);
    shot.damping_ratio = decimal_value(source.motion.damping_ratio);
    if (shot.motion == Motion::spring) {
        const double step{2.0 * pi * shot.frequency / frames_per_second(shots.clock)};
        if (!(step <= max_spring_step)) {
            messages.errors.push_back(
                path + ".motion.spring.frequency: the spring moves too far in one frame; 2 pi "
                       "times the frequency, divided by the frame rate, must be at most 0.5"
            );
            ok = false;
        }
        if (!(shot.damping_ratio * step <= max_spring_step)) {
            messages.errors.push_back(
                path + ".motion.spring.dampingRatio: the spring is damped too hard for one "
                       "frame; the damping ratio times 2 pi times the frequency, divided by the "
                       "frame rate, must be at most 0.5"
            );
            ok = false;
        }
    }

    if (source.transition) {
        shot.transition = source.transition->kind;
        if (index == 0) {
            messages.errors.push_back(
                path + ".transition: the first shot has no shot before it to take over from"
            );
            ok = false;
        } else {
            shot.transition_frames = frames_of_seconds(shots.clock, source.transition->duration);
            if (shot.transition_frames == 0) {
                messages.errors.push_back(
                    path + ".transition.duration: shorter than half a frame, so no frame shows it"
                );
                ok = false;
            }
        }
    }
    return ok;
}

} // namespace

double fit_height(MapBounds bounds, OutputSize output) noexcept {
    const double across{
        static_cast<double>(bounds.width) * static_cast<double>(output.height) /
        static_cast<double>(output.width)
    };
    const double down{static_cast<double>(bounds.height)};
    const double fit{across < down ? across : down};
    return fit > min_view_height ? fit : min_view_height;
}

View clamp_view(View view, MapBounds bounds, OutputSize output) noexcept {
    const double height{held_within(view.height, min_view_height, fit_height(bounds, output))};
    const double width{
        height * static_cast<double>(output.width) / static_cast<double>(output.height)
    };
    const double map_width{static_cast<double>(bounds.width)};
    const double map_height{static_cast<double>(bounds.height)};
    View kept{view.x, view.z, height, view.yaw, held_within(view.pitch, min_pitch, max_pitch)};
    if (width >= map_width)
        kept.x = map_width / 2.0;
    else
        kept.x = held_within(view.x, width / 2.0, map_width - width / 2.0);
    if (height >= map_height)
        kept.z = map_height / 2.0;
    else
        kept.z = held_within(view.z, height / 2.0, map_height - height / 2.0);
    return kept;
}

View view_of_camera(const CameraState& camera) noexcept {
    return View{
        decimal_value(camera.position.x),
        decimal_value(camera.position.z),
        decimal_value(camera.position.y),
        camera.orientation ? decimal_value(camera.orientation->yaw) : 0.0,
        camera.orientation ? decimal_value(camera.orientation->pitch) : 0.0,
    };
}

EngineView engine_view(View view, OutputSize output) noexcept {
    const double output_width{static_cast<double>(output.width)};
    const double output_height{static_cast<double>(output.height)};
    const double zoom{output_height / view.height};
    const double width{view.height * output_width / output_height};
    const double left_edge{view.x - width / 2.0};
    const double top_edge{view.z - view.height / 2.0};
    const double left{std::floor(left_edge)};
    const double top{std::floor(top_edge)};
    // ceil(zoom), as -floor(-zoom), at least 1.
    const uint32_t margin_pixels{to_uint32(-std::floor(-zoom))};
    const uint32_t margin{margin_pixels > 0 ? margin_pixels : 1};
    const uint32_t offset_x{to_uint32(std::floor((left_edge - left) * zoom))};
    const uint32_t offset_y{to_uint32(std::floor((top_edge - top) * zoom))};
    EngineView engine{};
    engine.left = to_int32(left);
    engine.top = to_int32(top);
    engine.visible_width = to_int32(std::floor(width + 0.5));
    engine.visible_height = to_int32(std::floor(view.height + 0.5));
    engine.zoom = zoom;
    engine.margin = margin;
    engine.offset_x = offset_x < margin ? offset_x : margin - 1;
    engine.offset_y = offset_y < margin ? offset_y : margin - 1;
    return engine;
}

bool compile_shots(
    const Script& script,
    MapBounds bounds,
    uint32_t recording_end_tick,
    ShotList& shots,
    CompileMessages& messages
) {
    shots = ShotList{};
    messages = CompileMessages{};
    ShotList list{};
    list.output = OutputSize{script.output.width, script.output.height};
    list.bounds = bounds;
    list.clock.tickrate = rational_of(script.input.tickrate);
    list.clock.framerate = rational_of(script.output.framerate);
    list.chunks =
        ChunkRule{script.output.chunking.mode, rational_of(script.output.chunking.length)};
    const auto& sources{script.director.shots};
    if (sources.empty()) {
        messages.errors.emplace_back("director.shots: the script has no shot");
        return false;
    }
    list.clock.first_tick = sources.front().tick;

    bool ok{true};
    const bool end_given{script.director.end_tick.has_value()};
    list.end_tick = end_given ? *script.director.end_tick : recording_end_tick;
    const uint32_t last_tick{sources.back().tick};
    bool end_known{true};
    if (list.end_tick <= last_tick) {
        messages.errors.push_back(
            (end_given ? std::string{"director.endTick: "}
                       : std::string{"director: the recording ends at tick "} +
                             std::to_string(list.end_tick) + ", so ") +
            "the video ends before the last shot's tick, " + std::to_string(last_tick)
        );
        ok = false;
        end_known = false;
    } else {
        list.frame_count = first_frame_of_tick(list.clock, list.end_tick);
        if (list.frame_count > max_frame_count) {
            messages.errors.push_back(
                std::string{end_given ? "director.endTick" : "director"} + ": the video has " +
                std::to_string(list.frame_count) + " frames, more than " +
                std::to_string(max_frame_count)
            );
            ok = false;
            end_known = false;
        }
    }

    list.shots.reserve(sources.size());
    for (size_t index{}; index < sources.size(); ++index) {
        CompiledShot shot{};
        ok = compile_shot_motion(list, sources[index], index, shot, messages) && ok;
        shot.first_frame = first_frame_of_tick(list.clock, sources[index].tick);
        const bool last{index + 1 == sources.size()};
        if (!last || end_known) {
            const uint64_t next{
                last ? list.frame_count : first_frame_of_tick(list.clock, sources[index + 1].tick)
            };
            shot.frame_count = next > shot.first_frame ? next - shot.first_frame : 0;
            if (shot.frame_count == 0) {
                messages.errors.push_back(shot_path(index) + ": no frame shows this shot");
                ok = false;
            } else if (shot.transition_frames > shot.frame_count) {
                messages.errors.push_back(
                    shot_path(index) +
                    ".transition.duration: " + std::to_string(shot.transition_frames) +
                    " frames, longer than the shot's " + std::to_string(shot.frame_count)
                );
                ok = false;
            }
        }
        list.shots.push_back(shot);
    }
    if (!ok)
        return false;
    shots = std::move(list);
    return true;
}

CameraRig::CameraRig(const ShotList& shots) : shots_{&shots} {
    if (shots.shots.empty())
        return;
    const CompiledShot& first{shots.shots.front()};
    const double fps{frames_per_second(shots.clock)};
    begin_axis(first, first.start.x, first.end.x, fps, x_.value, x_.speed);
    begin_axis(first, first.start.z, first.end.z, fps, z_.value, z_.speed);
    begin_axis(
        first,
        1.0 / first.start.height,
        1.0 / first.end.height,
        fps,
        inverse_height_.value,
        inverse_height_.speed
    );
    begin_axis(first, first.start.yaw, first.end.yaw, fps, yaw_.value, yaw_.speed);
    begin_axis(first, first.start.pitch, first.end.pitch, fps, pitch_.value, pitch_.speed);
}

bool CameraRig::next(FrameCameras& cameras) {
    const ShotList& list{*shots_};
    if (frame_ >= list.frame_count || list.shots.empty())
        return false;
    const double fps{frames_per_second(list.clock)};
    while (shot_ + 1 < list.shots.size() && frame_ >= list.shots[shot_ + 1].first_frame) {
        const CompiledShot& ending{list.shots[shot_]};
        end_axis(ending, ending.end.x, x_.value);
        end_axis(ending, ending.end.z, z_.value);
        end_axis(ending, 1.0 / ending.end.height, inverse_height_.value);
        end_axis(ending, ending.end.yaw, yaw_.value);
        end_axis(ending, ending.end.pitch, pitch_.value);
        held_ = View{x_.value, z_.value, 1.0 / inverse_height_.value, yaw_.value, pitch_.value};
        ++shot_;
        const CompiledShot& starting{list.shots[shot_]};
        begin_axis(starting, starting.start.x, starting.end.x, fps, x_.value, x_.speed);
        begin_axis(starting, starting.start.z, starting.end.z, fps, z_.value, z_.speed);
        begin_axis(
            starting,
            1.0 / starting.start.height,
            1.0 / starting.end.height,
            fps,
            inverse_height_.value,
            inverse_height_.speed
        );
        begin_axis(starting, starting.start.yaw, starting.end.yaw, fps, yaw_.value, yaw_.speed);
        begin_axis(
            starting, starting.start.pitch, starting.end.pitch, fps, pitch_.value, pitch_.speed
        );
    }

    const CompiledShot& shot{list.shots[shot_]};
    const uint64_t step{frame_ - shot.first_frame};
    View view{x_.value, z_.value, 1.0 / inverse_height_.value, yaw_.value, pitch_.value};
    if (shot.motion == Motion::linear) {
        view.x = linear_value(x_.value, shot.end.x, step, shot.frame_count);
        view.z = linear_value(z_.value, shot.end.z, step, shot.frame_count);
        view.height =
            1.0 /
            linear_value(inverse_height_.value, 1.0 / shot.end.height, step, shot.frame_count);
        view.yaw = linear_value(yaw_.value, shot.end.yaw, step, shot.frame_count);
        view.pitch = linear_value(pitch_.value, shot.end.pitch, step, shot.frame_count);
    }

    cameras = FrameCameras{};
    cameras.frame = frame_;
    cameras.tick = frame_tick(list.clock, frame_);
    cameras.shot = shot_;
    cameras.camera = clamp_view(view, list.bounds, list.output);
    if (step < shot.transition_frames) {
        cameras.transition = true;
        cameras.outgoing = clamp_view(held_, list.bounds, list.output);
        cameras.transition_kind = shot.transition;
        cameras.transition_step = static_cast<uint32_t>(step);
        cameras.transition_steps = static_cast<uint32_t>(shot.transition_frames);
    }

    if (shot.motion == Motion::spring) {
        const double frame_length{frame_seconds(list.clock)};
        step_spring(spring_of(shot, shot.end.x, frame_length), x_.value, x_.speed);
        step_spring(spring_of(shot, shot.end.z, frame_length), z_.value, z_.speed);
        step_spring(
            spring_of(shot, 1.0 / shot.end.height, frame_length),
            inverse_height_.value,
            inverse_height_.speed
        );
        step_spring(spring_of(shot, shot.end.yaw, frame_length), yaw_.value, yaw_.speed);
        step_spring(spring_of(shot, shot.end.pitch, frame_length), pitch_.value, pitch_.speed);
    }
    ++frame_;
    return true;
}

uint64_t CameraRig::position() const noexcept {
    return frame_;
}

} // namespace oa::media::director
