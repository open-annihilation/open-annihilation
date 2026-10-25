// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The director's camera: fitting and clamping views on four reference maps,
// the engine camera with its sub-pixel offsets and margin, compiling a
// script's shots with each error and warning, linear and spring motion,
// continuation and transitions, and a pinned digest of 600 frames of
// cameras, which must be the same on every platform.

#include "oa/base/sha256.hpp"
#include "oa/media/director.hpp"
#include "oa/platform/system.hpp"
#include "oa/platform/files.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace director = oa::media::director;
namespace oascript = oa::formats::oascript;

namespace {

int failures = 0;

/// Records a failed expectation with its line.
void expect(
    bool value,
    std::string_view what,
    const std::source_location where = std::source_location::current()
) {
    if (value)
        return;
    ++failures;
    std::cerr << where.file_name() << ':' << where.line() << ": " << what << '\n';
}

/// Coast To Coast's camera bounds, Game.map_pixel_width by map_pixel_height.
constexpr director::MapBounds coast_to_coast{3328, 1888};
/// Show Down's camera bounds.
constexpr director::MapBounds show_down{5120, 2048};
/// Painted Desert's camera bounds.
constexpr director::MapBounds painted_desert{9216, 9216};
/// Caldera's Rim's camera bounds.
constexpr director::MapBounds calderas_rim{14816, 14720};
/// 1080p and 4K outputs.
constexpr director::OutputSize full_hd{1920, 1080};
constexpr director::OutputSize ultra_hd{3840, 2160};

/// Returns a camera at a centre and view height, each a decimal of tenths.
oascript::CameraState camera_tenths(int64_t x, int64_t z, int64_t height) {
    oascript::CameraState camera{};
    camera.position.x = oascript::Decimal{x, 1};
    camera.position.y = oascript::Decimal{height, 1};
    camera.position.z = oascript::Decimal{z, 1};
    return camera;
}

/// Returns a camera at a whole-pixel centre and view height.
oascript::CameraState camera_at(int64_t x, int64_t z, int64_t height) {
    return camera_tenths(x * 10, z * 10, height * 10);
}

/// Returns a shot moving to a camera, linearly unless a spring is given.
oascript::Shot
shot_to(uint32_t tick, std::optional<oascript::CameraState> start, oascript::CameraState end) {
    oascript::Shot shot{};
    shot.tick = tick;
    shot.camera_start = start;
    shot.camera_end = end;
    return shot;
}

/// Returns a spring motion from decimal frequency and damping ratio.
oascript::ShotMotion spring(oascript::Decimal frequency, oascript::Decimal damping_ratio) {
    return oascript::ShotMotion{oascript::Motion::spring, frequency, damping_ratio};
}

/// Returns a script at 60 frames and 30 ticks a second.
oascript::Script script_with(std::vector<oascript::Shot> shots, std::optional<uint32_t> end_tick) {
    oascript::Script script{};
    script.input.recording = "game.rec";
    script.director.shots = std::move(shots);
    script.director.end_tick = end_tick;
    return script;
}

/// Returns true when some message starts with a text.
bool has_message(const std::vector<std::string>& messages, std::string_view start) {
    for (const std::string& message : messages)
        if (message.starts_with(start))
            return true;
    return false;
}

/// Compiles a script, expecting failure with an error starting with a text.
void expect_error(
    const oascript::Script& script,
    director::MapBounds bounds,
    uint32_t recording_end_tick,
    std::string_view start,
    const std::source_location where = std::source_location::current()
) {
    director::ShotList shots{};
    director::CompileMessages messages{};
    expect(
        !director::compile_shots(script, bounds, recording_end_tick, shots, messages),
        "compile fails",
        where
    );
    expect(has_message(messages.errors, start), start, where);
    expect(shots.shots.empty() && shots.frame_count == 0, "nothing compiled", where);
}

/// Compiles a script that must compile.
director::ShotList compiled(
    const oascript::Script& script,
    director::MapBounds bounds,
    const std::source_location where = std::source_location::current()
) {
    director::ShotList shots{};
    director::CompileMessages messages{};
    const bool ok{director::compile_shots(script, bounds, 0, shots, messages)};
    expect(ok, "compile succeeds", where);
    for (const std::string& error : messages.errors)
        std::cerr << "  " << error << '\n';
    return shots;
}

/// Returns every frame's cameras of a compiled script.
std::vector<director::FrameCameras> all_frames(const director::ShotList& shots) {
    std::vector<director::FrameCameras> frames{};
    director::CameraRig rig{shots};
    director::FrameCameras cameras{};
    while (rig.next(cameras))
        frames.push_back(cameras);
    return frames;
}

void test_fit_and_clamp() {
    for (const director::OutputSize output : {full_hd, ultra_hd}) {
        expect(
            director::fit_height(coast_to_coast, output) == 1872.0, "Coast To Coast fits at 1872"
        );
        expect(director::fit_height(show_down, output) == 2048.0, "Show Down fits at 2048");
        expect(
            director::fit_height(painted_desert, output) == 5184.0, "Painted Desert fits at 5184"
        );
        expect(director::fit_height(calderas_rim, output) == 8334.0, "Caldera's Rim fits at 8334");
    }
    expect(director::fit_height({200, 100}, full_hd) == director::min_view_height, "a tiny map");
    expect(director::fit_height(painted_desert, {1080, 1920}) == 9216.0, "an upright output");

    const director::View whole{
        director::clamp_view({100.0, 100.0, 5000.0}, coast_to_coast, full_hd)
    };
    expect(whole.height == 1872.0 && whole.x == 1664.0 && whole.z == 936.0, "whole map, too high");
    const director::View corner{
        director::clamp_view({3000.0, 1800.0, 1080.0}, coast_to_coast, ultra_hd)
    };
    expect(
        corner.x == 2368.0 && corner.z == 1348.0 && corner.height == 1080.0, "held in the corner"
    );
    const director::View low{director::clamp_view({1000.0, 900.0, 100.0}, coast_to_coast, full_hd)};
    expect(low.height == director::min_view_height && low.x == 1000.0 && low.z == 900.0, "raised");
    const director::View edge{director::clamp_view({10.0, 5000.0, 2048.0}, show_down, full_hd)};
    expect(edge.z == 1024.0 && edge.x == 1820.4444444444443, "as high as the map: centred down");
    const director::View tiny{director::clamp_view({0.0, 0.0, 1000.0}, {200, 100}, full_hd)};
    expect(tiny.x == 100.0 && tiny.z == 50.0 && tiny.height == 270.0, "a tiny map is centred");
    const director::View inside{
        director::clamp_view({5000.5, 7000.25, 3000.0}, calderas_rim, ultra_hd)
    };
    expect(inside.x == 5000.5 && inside.z == 7000.25 && inside.height == 3000.0, "inside is kept");
    const director::View broken{director::clamp_view({NAN, NAN, NAN}, painted_desert, full_hd)};
    expect(broken.height == 270.0 && broken.x == 240.0 && broken.z == 135.0, "not a number");
}

void test_view_of_camera() {
    oascript::CameraState camera{};
    camera.position.x = oascript::Decimal{12345, 1};
    camera.position.y = oascript::Decimal{1080, 0};
    camera.position.z = oascript::Decimal{-5, 0};
    camera.orientation = oascript::CameraOrientation{{10, 0}, {20, 0}, {30, 0}};
    const director::View view{director::view_of_camera(camera)};
    expect(view.x == 1234.5 && view.height == 1080.0 && view.z == -5.0, "view of a camera");
    camera.position.x = oascript::Decimal{1, 3};
    expect(director::view_of_camera(camera).x == 0.001, "thousandths");
}

void test_engine_view() {
    const director::EngineView half{director::engine_view({1000.25, 600.75, 1080.0}, ultra_hd)};
    expect(half.zoom == 2.0 && half.left == 40 && half.top == 60, "zoom 2: corner");
    expect(half.visible_width == 1920 && half.visible_height == 1080, "zoom 2: size");
    expect(half.margin == 2 && half.offset_x == 0 && half.offset_y == 1, "zoom 2: offsets");

    const director::EngineView odd{director::engine_view({1000.5, 700.95, 1000.0}, full_hd)};
    expect(
        odd.zoom == 1.08 && odd.visible_width == 1778 && odd.visible_height == 1000, "zoom 1.08"
    );
    expect(odd.left == 111 && odd.top == 200, "zoom 1.08: corner");
    expect(odd.margin == 2 && odd.offset_x == 0 && odd.offset_y == 1, "zoom 1.08: offsets");

    const director::EngineView wide{director::engine_view({1664.0, 944.0, 2160.0}, full_hd)};
    expect(
        wide.zoom == 0.5 && wide.margin == 1 && wide.offset_x == 0 && wide.offset_y == 0, "zoom 0.5"
    );
    expect(wide.left == -256 && wide.top == -136 && wide.visible_width == 3840, "zoom 0.5: corner");

    const director::EngineView close{director::engine_view({1270.999, 300.5, 540.0}, ultra_hd)};
    expect(close.zoom == 4.0 && close.margin == 4, "zoom 4");
    expect(close.left == 790 && close.offset_x == 3, "zoom 4: offset at most margin - 1");
    expect(close.top == 30 && close.offset_y == 2, "zoom 4: half a pixel");
}

void test_compile_errors() {
    const oascript::CameraState here{camera_at(1000, 800, 1080)};
    const oascript::CameraState there{camera_at(2000, 1000, 1200)};

    expect_error(
        script_with({shot_to(0, here, there), shot_to(100, {}, here)}, 100),
        coast_to_coast,
        0,
        "director.endTick: "
    );
    expect_error(
        script_with({shot_to(0, here, there)}, {}),
        coast_to_coast,
        0,
        "director: the recording ends at tick 0"
    );
    expect_error(script_with({}, 10), coast_to_coast, 0, "director.shots: ");
    expect_error(
        script_with({shot_to(0, {}, there)}, 10),
        coast_to_coast,
        0,
        "director.shots[0].cameraStart: "
    );

    oascript::Script slow{script_with({shot_to(0, here, there)}, 10000)};
    slow.input.tickrate = oascript::Decimal{1, 3};
    slow.output.framerate = oascript::Decimal{240, 0};
    expect_error(slow, coast_to_coast, 0, "director.endTick: the video has 2400000000 frames");

    oascript::Script fast{
        script_with({shot_to(0, here, there), shot_to(2, {}, here), shot_to(3, {}, there)}, 100)
    };
    fast.input.tickrate = oascript::Decimal{300, 0};
    expect_error(fast, coast_to_coast, 0, "director.shots[1]: no frame shows this shot");

    oascript::Shot long_transition{shot_to(15, here, there)};
    long_transition.transition = oascript::Transition{oascript::TransitionKind::fade, {1, 0}};
    expect_error(
        script_with({shot_to(0, here, there), long_transition}, 30),
        coast_to_coast,
        0,
        "director.shots[1].transition.duration: 60 frames, longer than the shot's 30"
    );
    long_transition.tick = 70;
    long_transition.transition->duration = oascript::Decimal{1, 3};
    expect_error(
        script_with({shot_to(0, here, there), long_transition}, 100),
        coast_to_coast,
        0,
        "director.shots[1].transition.duration: shorter than half a frame"
    );
    oascript::Shot first_transition{shot_to(0, here, there)};
    first_transition.transition = oascript::Transition{oascript::TransitionKind::wipe, {5, 1}};
    expect_error(
        script_with({first_transition}, 100), coast_to_coast, 0, "director.shots[0].transition: "
    );

    oascript::Shot springy{shot_to(0, here, there)};
    springy.motion = spring({5, 0}, {1, 0});
    expect_error(
        script_with({springy}, 100),
        coast_to_coast,
        0,
        "director.shots[0].motion.spring.frequency: "
    );
    springy.motion = spring({4, 0}, {2, 0});
    expect_error(
        script_with({springy}, 100),
        coast_to_coast,
        0,
        "director.shots[0].motion.spring.dampingRatio: "
    );
    // A damping-blind bound would pass this spring, which diverges.
    springy.motion = spring({1, 0}, {10, 0});
    expect_error(
        script_with({springy}, 100),
        coast_to_coast,
        0,
        "director.shots[0].motion.spring.dampingRatio: "
    );
    springy.motion = spring({4, 1}, {10, 0});
    expect(
        !compiled(script_with({springy}, 100), coast_to_coast).shots.empty(),
        "a stiff damped spring within the bound"
    );

    // Every error is reported, not only the first.
    director::ShotList shots{};
    director::CompileMessages messages{};
    oascript::Shot bad_spring{shot_to(50, {}, there)};
    bad_spring.motion = spring({10, 0}, {1, 0});
    expect(
        !director::compile_shots(
            script_with({shot_to(0, {}, here), bad_spring}, 100), coast_to_coast, 0, shots, messages
        ),
        "several errors"
    );
    expect(messages.errors.size() == 3, "three errors");
}

void test_compile() {
    oascript::Script script{script_with(
        {shot_to(0, camera_at(1000, 800, 1080), camera_at(2000, 1000, 1200)),
         shot_to(45, {}, camera_at(3200, 1800, 100)),
         shot_to(100, camera_at(500, 500, 900), camera_at(1000, 600, 1080))},
        {}
    )};
    script.director.shots[2].motion = spring({5, 1}, {1, 0});
    script.director.shots[2].transition =
        oascript::Transition{oascript::TransitionKind::checkerboard, {25, 2}};
    script.output.chunking = oascript::Chunking{oascript::ChunkMode::ticks, {1800, 0}};
    director::ShotList shots{};
    director::CompileMessages messages{};
    expect(director::compile_shots(script, coast_to_coast, 250, shots, messages), "compiles");
    expect(shots.end_tick == 250 && shots.frame_count == 500, "ends with the recording");
    expect(shots.clock.first_tick == 0 && shots.clock.framerate.numerator == 60, "clock");
    expect(
        shots.chunks.mode == oascript::ChunkMode::ticks && shots.chunks.length.numerator == 1800,
        "chunks"
    );
    expect(shots.output.width == 1920 && shots.bounds.width == 3328, "output and bounds");
    expect(shots.shots.size() == 3, "three shots");
    expect(shots.shots[0].first_frame == 0 && shots.shots[0].frame_count == 90, "shot 0 frames");
    expect(shots.shots[1].first_frame == 90 && shots.shots[1].frame_count == 110, "shot 1 frames");
    expect(shots.shots[1].continues && !shots.shots[0].continues, "continuation");
    expect(shots.shots[2].first_frame == 200 && shots.shots[2].frame_count == 300, "shot 2 frames");
    expect(shots.shots[2].transition_frames == 15, "0.25 s is 15 frames");
    expect(shots.shots[2].transition == oascript::TransitionKind::checkerboard, "transition kind");
    expect(
        shots.shots[2].motion == oascript::Motion::spring && shots.shots[2].frequency == 0.5,
        "spring"
    );
    expect(shots.shots[2].damping_ratio == 1.0 && shots.shots[2].script_index == 2, "damping");
    expect(shots.shots[2].start.x == 500.0 && shots.shots[2].end.height == 1080.0, "cameras");
    expect(messages.errors.empty(), "no error");
    expect(messages.warnings.size() == 3, "three warnings");
    expect(has_message(messages.warnings, "director.shots[1].cameraEnd.position.y: "), "too low");
    expect(
        has_message(messages.warnings, "director.shots[1].cameraEnd.position: "), "past the edge"
    );
    expect(
        has_message(messages.warnings, "director.shots[2].cameraStart.position: "), "past the edge"
    );

    // An endTick given wins over the recording's end.
    script.director.end_tick = 200;
    expect(director::compile_shots(script, coast_to_coast, 250, shots, messages), "compiles");
    expect(shots.end_tick == 200 && shots.frame_count == 400, "ends at endTick");

    // Tick rate 45 at 24 frames a second, starting at tick 30.
    oascript::Script odd{script_with(
        {shot_to(30, camera_at(1000, 800, 1080), camera_at(2000, 1000, 1200)),
         shot_to(76, {}, camera_at(1500, 900, 1300))},
        120
    )};
    odd.input.tickrate = oascript::Decimal{45, 0};
    odd.output.framerate = oascript::Decimal{24, 0};
    const director::ShotList odd_shots{compiled(odd, coast_to_coast)};
    expect(odd_shots.clock.first_tick == 30 && odd_shots.frame_count == 48, "odd rates: frames");
    expect(
        odd_shots.shots.size() == 2 && odd_shots.shots[1].first_frame == 25, "odd rates: shot 1"
    );
}

void test_linear() {
    const director::ShotList shots{compiled(
        script_with(
            {shot_to(0, camera_at(1000, 600, 1080), camera_at(2000, 1000, 1200)),
             shot_to(60, {}, camera_at(1500, 800, 1500))},
            90
        ),
        coast_to_coast
    )};
    const std::vector<director::FrameCameras> frames{all_frames(shots)};
    expect(frames.size() == 180, "180 frames");
    if (frames.size() != 180)
        return;
    expect(frames[0].camera.x == 1000.0 && frames[0].camera.z == 600.0, "linear start");
    expect(frames[0].camera.height == 1.0 / (1.0 / 1080.0), "linear start height");
    expect(frames[0].tick == 0 && frames[1].tick == 0 && frames[2].tick == 1, "ticks");
    expect(frames[60].camera.x == 1500.0 && frames[60].camera.z == 800.0, "half way");
    // The continuing shot starts exactly where the first one ends.
    expect(frames[120].shot == 1 && !frames[120].transition, "shot 1");
    expect(frames[120].camera.x == 2000.0 && frames[120].camera.z == 1000.0, "boundary");
    expect(frames[120].camera.height == 1.0 / (1.0 / 1200.0), "boundary height");
    expect(frames[119].camera.x < 2000.0 && frames[119].camera.x > 1990.0, "before the boundary");
    // 1 / height changes evenly.
    const double first_step{1.0 / frames[1].camera.height - 1.0 / frames[0].camera.height};
    const double later_step{1.0 / frames[101].camera.height - 1.0 / frames[100].camera.height};
    expect(std::fabs(first_step - later_step) < 1e-15, "1 / height changes evenly");
    for (size_t frame{}; frame < frames.size(); ++frame)
        expect(frames[frame].frame == frame, "frames in order");
}

void test_spring() {
    oascript::Shot settle{shot_to(0, camera_at(1000, 600, 1080), camera_at(2000, 1000, 1200))};
    settle.motion = spring({1, 0}, {1, 0});
    const director::ShotList shots{compiled(script_with({settle}, 300), coast_to_coast)};
    const std::vector<director::FrameCameras> frames{all_frames(shots)};
    expect(frames.size() == 600, "600 frames");
    if (frames.size() != 600)
        return;
    expect(
        frames[0].camera.x == 1000.0 && frames[1].camera.x > 1000.0, "the spring starts at rest"
    );
    bool rising{true};
    for (size_t frame{1}; frame < frames.size(); ++frame)
        rising = rising && frames[frame].camera.x >= frames[frame - 1].camera.x &&
                 frames[frame].camera.x <= 2000.0 && frames[frame].camera.z <= 1000.0;
    expect(rising, "no overshoot at a damping ratio of 1");
    expect(std::fabs(frames.back().camera.x - 2000.0) < 1e-6, "it settles");
    expect(std::fabs(frames.back().camera.height - 1200.0) < 1e-6, "its height settles");
}

/// Returns a camera turned by a yaw and a pitch, in tenths of a degree.
oascript::CameraState
turned_camera(oascript::CameraState camera, int64_t yaw_tenths, int64_t pitch_tenths) {
    camera.orientation = oascript::CameraOrientation{{pitch_tenths, 1}, {yaw_tenths, 1}, {0, 0}};
    return camera;
}

void test_turned() {
    // A camera's yaw and pitch, roll left out; none without an orientation.
    const director::View view{
        director::view_of_camera(turned_camera(camera_at(1000, 600, 1080), -305, 100))
    };
    expect(view.yaw == -30.5 && view.pitch == 10.0, "a camera's yaw and pitch");
    expect(director::turned(view), "a turned view");
    const director::View plain{director::view_of_camera(camera_at(1000, 600, 1080))};
    expect(plain.yaw == 0.0 && plain.pitch == 0.0 && !director::turned(plain), "no orientation");
    // The pitch is held to the unlocked range; the yaw is kept as it is.
    const director::View high{director::clamp_view(
        {1000.0, 600.0, 1080.0, 720.0, 40.0}, coast_to_coast, director::OutputSize{1920, 1080}
    )};
    expect(high.pitch == director::max_pitch && high.yaw == 720.0, "pitch held, yaw kept");
    const director::View low{director::clamp_view(
        {1000.0, 600.0, 1080.0, 0.0, -80.0}, coast_to_coast, director::OutputSize{1920, 1080}
    )};
    expect(low.pitch == director::min_pitch, "pitch held from below");
    expect(
        director::min_pitch + director::classic_elevation == director::min_elevation &&
            director::max_pitch + director::classic_elevation == director::max_elevation,
        "the pitch range is the elevation range"
    );

    // A linear shot turns evenly, and a continuing one turns on from where
    // it ended; a camera with no orientation turns back to the classic view.
    oascript::Script script{script_with(
        {shot_to(
             0,
             turned_camera(camera_at(1000, 600, 1080), -300, 0),
             turned_camera(camera_at(1000, 600, 1080), 300, 100)
         ),
         shot_to(60, {}, camera_at(1000, 600, 1080))},
        90
    )};
    director::ShotList shots{};
    director::CompileMessages messages{};
    expect(director::compile_shots(script, coast_to_coast, 0, shots, messages), "turned compiles");
    expect(messages.warnings.empty(), "turning within the range warns about nothing");
    const std::vector<director::FrameCameras> frames{all_frames(shots)};
    expect(frames.size() == 180, "180 turned frames");
    if (frames.size() == 180) {
        expect(frames[0].camera.yaw == -30.0 && frames[0].camera.pitch == 0.0, "turned start");
        expect(frames[60].camera.yaw == 0.0 && frames[60].camera.pitch == 5.0, "half way round");
        expect(frames[120].camera.yaw == 30.0 && frames[120].camera.pitch == 10.0, "turned end");
        expect(frames[179].camera.yaw > 0.0 && frames[179].camera.yaw < 1.0, "turning back");
    }

    // A pitch outside the range warns and is held; a spring turns the camera
    // as it moves the view.
    script.director.shots[0].camera_end = turned_camera(camera_at(1000, 600, 1080), 0, 400);
    script.director.shots[1].camera_end = turned_camera(camera_at(1000, 600, 1080), 900, 0);
    script.director.shots[1].motion = spring({1, 0}, {1, 0});
    expect(director::compile_shots(script, coast_to_coast, 0, shots, messages), "spring compiles");
    expect(
        messages.warnings.size() == 1 &&
            has_message(messages.warnings, "director.shots[0].cameraEnd.orientation.pitch: "),
        "a pitch above the range warns"
    );
    const std::vector<director::FrameCameras> sprung{all_frames(shots)};
    if (sprung.size() == 180) {
        expect(sprung[119].camera.pitch == director::max_pitch, "the pitch is held at its top");
        bool rising{true};
        for (size_t frame{121}; frame < sprung.size(); ++frame)
            rising = rising && sprung[frame].camera.yaw > sprung[frame - 1].camera.yaw &&
                     sprung[frame].camera.yaw < 90.0;
        expect(rising, "the spring turns the camera towards its end");
    }
}

void test_continuation() {
    oascript::Shot first{shot_to(0, camera_at(1000, 600, 1080), camera_at(2000, 1000, 1200))};
    first.motion = spring({5, 1}, {1, 0});
    oascript::Shot second{shot_to(30, {}, camera_at(2100, 1000, 1200))};
    second.motion = spring({5, 1}, {1, 0});
    oascript::Shot third{shot_to(60, {}, camera_at(2100, 1000, 1200))};
    const std::vector<director::FrameCameras> frames{
        all_frames(compiled(script_with({first, second, third}, 90), coast_to_coast))
    };
    expect(frames.size() == 180, "180 frames");
    if (frames.size() != 180)
        return;
    // Across the cut at frame 60 the step per frame changes by no more than
    // one frame's acceleration: the speed is carried.
    const double before{frames[59].camera.x - frames[58].camera.x};
    const double across{frames[60].camera.x - frames[59].camera.x};
    const double after{frames[61].camera.x - frames[60].camera.x};
    expect(before > 1.0 && std::fabs(across - before) < 0.1 * before, "no jump in value");
    expect(std::fabs(after - across) < 0.1 * before, "no jump in speed");
    expect(!frames[60].transition, "a cut, not a transition");
    // The linear shot after it starts from the spring's end state.
    const double spring_end{frames[119].camera.x + (frames[119].camera.x - frames[118].camera.x)};
    expect(std::fabs(frames[120].camera.x - spring_end) < 1.0, "linear continues from the spring");
    expect(frames[179].camera.x < 2100.0, "linear at its own speed");
}

void test_transition() {
    oascript::Shot incoming{shot_to(60, camera_at(2000, 1000, 1080), camera_at(2200, 1100, 1080))};
    incoming.transition = oascript::Transition{oascript::TransitionKind::dissolve, {5, 1}};
    const std::vector<director::FrameCameras> frames{all_frames(compiled(
        script_with(
            {shot_to(0, camera_at(1000, 600, 1080), camera_at(1500, 800, 1080)), incoming}, 120
        ),
        coast_to_coast
    ))};
    expect(frames.size() == 240, "240 frames");
    if (frames.size() != 240)
        return;
    expect(!frames[119].transition, "no transition before the shot");
    for (uint32_t step{}; step < 30; ++step) {
        const director::FrameCameras& frame{frames[120 + step]};
        expect(
            frame.transition && frame.transition_step == step && frame.transition_steps == 30,
            "transition steps"
        );
        expect(
            frame.outgoing.x == 1500.0 && frame.outgoing.z == 800.0, "the outgoing view is held"
        );
        expect(frame.transition_kind == oascript::TransitionKind::dissolve, "kind");
    }
    expect(
        frames[120].camera.x == 2000.0 && frames[121].camera.x > 2000.0, "the incoming view moves"
    );
    expect(!frames[150].transition, "the transition ends");
}

/// Appends a double's bits as 16 hexadecimal digits and a space.
void append_bits(std::string& text, double value) {
    char digits[24]{};
    std::snprintf(
        digits,
        sizeof digits,
        "%016llx ",
        static_cast<unsigned long long>(std::bit_cast<uint64_t>(value))
    );
    text += digits;
}

/// Appends a whole number and a space.
void append_number(std::string& text, int64_t value) {
    text += std::to_string(value);
    text += ' ';
}

/// Appends a frame's cameras and the engine camera of its view as a line.
void append_frame(
    std::string& text, const director::FrameCameras& frame, director::OutputSize output
) {
    append_number(text, static_cast<int64_t>(frame.frame));
    append_number(text, frame.tick);
    append_number(text, static_cast<int64_t>(frame.shot));
    append_bits(text, frame.camera.x);
    append_bits(text, frame.camera.z);
    append_bits(text, frame.camera.height);
    append_number(text, frame.transition ? 1 : 0);
    append_bits(text, frame.outgoing.x);
    append_bits(text, frame.outgoing.z);
    append_bits(text, frame.outgoing.height);
    append_number(text, static_cast<int64_t>(frame.transition_kind));
    append_number(text, frame.transition_step);
    append_number(text, frame.transition_steps);
    const director::EngineView engine{director::engine_view(frame.camera, output)};
    append_number(text, engine.left);
    append_number(text, engine.top);
    append_number(text, engine.visible_width);
    append_number(text, engine.visible_height);
    append_bits(text, engine.zoom);
    append_number(text, engine.offset_x);
    append_number(text, engine.offset_y);
    append_number(text, engine.margin);
    text += '\n';
}

/// The SHA-256 of the pinned script's 600 frames, one line each: frame, tick,
/// shot, the view's x, z and height, the transition flag, the outgoing view,
/// the transition's kind, step and steps, then the engine view's left, top,
/// visible width and height, zoom, offsets and margin. Doubles are written as
/// the 16 hexadecimal digits of their bits, whole numbers in decimal, each
/// followed by a space. Set OA_DIRECTOR_CAMERAS_TEXT to a path to have a
/// failing run write the text there.
constexpr std::string_view pinned_cameras_digest{
    "4a161abe49554df60f80d7c7fc7a57753d41fd006ec29459601ab9d426df819c"
};

void test_pinned_cameras() {
    oascript::Shot wide{shot_to(140, camera_at(1664, 944, 1872), camera_at(1000, 600, 540))};
    wide.motion = spring({1, 0}, {4, 1});
    wide.transition = oascript::Transition{oascript::TransitionKind::dissolve, {5, 1}};
    oascript::Shot follow{shot_to(60, {}, camera_at(2400, 1200, 1400))};
    follow.motion = spring({5, 1}, {1, 0});
    oascript::Shot away{shot_to(220, {}, camera_at(3000, 1700, 900))};
    away.transition = oascript::Transition{oascript::TransitionKind::wipe, {25, 2}};
    oascript::Script script{script_with(
        {shot_to(0, camera_at(800, 700, 1080), camera_tenths(15005, 9002, 8000)),
         follow,
         wide,
         away},
        300
    )};
    script.output.width = ultra_hd.width;
    script.output.height = ultra_hd.height;
    const director::ShotList shots{compiled(script, coast_to_coast)};
    const std::vector<director::FrameCameras> frames{all_frames(shots)};
    expect(frames.size() == 600, "600 frames");
    std::string text{};
    for (const director::FrameCameras& frame : frames)
        append_frame(text, frame, ultra_hd);
    const oa::base::sha256::Digest digest{oa::base::sha256::digest_of(
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(text.data()), text.size()}
    )};
    const std::array<char, oa::base::sha256::hex_size> digits{oa::base::sha256::to_hex(digest)};
    const std::string hex{digits.data(), digits.size()};
    if (hex != pinned_cameras_digest) {
        std::cerr << "cameras digest " << hex << '\n';
        if (const auto path{oa::platform::environment_value("OA_DIRECTOR_CAMERAS_TEXT")}) {
            if (std::FILE * file{oa::platform::open_file(path->c_str(), "wb")}) {
                std::fwrite(text.data(), 1, text.size(), file);
                std::fclose(file);
            }
        }
    }
    expect(hex == pinned_cameras_digest, "pinned cameras digest");
}

} // namespace

int main() {
    test_fit_and_clamp();
    test_view_of_camera();
    test_engine_view();
    test_compile_errors();
    test_compile();
    test_linear();
    test_spring();
    test_continuation();
    test_turned();
    test_transition();
    test_pinned_cameras();
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    return 0;
}
