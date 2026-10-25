// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The typed director script: decoding with defaults, every error and warning
// with its key path and position, encoding in the documented key order, the
// writers' exact bytes, and round trips through both forms.

#include "check.hpp"

#include "oa/formats/oascript.hpp"

#include <cstdio>
#include <string>
#include <string_view>

namespace {

using namespace oa::formats::oascript;
using oa::formats::oascript::test::bytes_of;
using oa::formats::oascript::test::same_tree;

/// The smallest valid script, relying on every default.
constexpr std::string_view minimal_yaml{R"(oascript: 1
input:
  demo: game.rec
director:
  shots:
    - tick: 0
      cameraStart: { position: { x: 100, y: 200, z: 300 } }
      cameraEnd: { position: { x: 100, y: 200, z: 300 } }
)"};

/// A script that sets every key.
constexpr std::string_view full_yaml{R"(oascript: 1
input:
  demo: replays/game.rec
  tickrate: 45.5
output:
  resolution: { width: 3840, height: 2160 }
  framerate: 59.94
  chunking: { mode: ticks, length: 900 }
  showUx: true
director:
  shots:
    - tick: 0
      cameraStart:
        position: { x: 1024, y: 2048, z: 1024 }
      cameraEnd:
        position: { x: 512.5, y: 640, z: 700 }
        orientation: { pitch: 0, yaw: 0, roll: 0 }
      motion: { linear: {} }
    - tick: 900
      cameraEnd:
        position: { x: 300, y: 480, z: 1200.25 }
      motion:
        spring: { frequency: 0.5, dampingRatio: 0.75 }
      transition: { type: checkerboard, duration: 0.25 }
    - tick: 1500
      cameraStart:
        position: { x: -10, y: 0.5, z: 0 }
      cameraEnd:
        position: { x: 20, y: 1, z: 40 }
      transition: { type: fade, duration: 2 }
  endTick: 1800
)"};

/// The same script as full_yaml in JSON.
constexpr std::string_view full_json{R"({
  "oascript": 1,
  "input": {"demo": "replays/game.rec", "tickrate": 45.5},
  "output": {
    "resolution": {"width": 3840, "height": 2160},
    "framerate": 59.94,
    "chunking": {"mode": "ticks", "length": 900},
    "showUx": true
  },
  "director": {
    "shots": [
      {
        "tick": 0,
        "cameraStart": {"position": {"x": 1024, "y": 2048, "z": 1024}},
        "cameraEnd": {
          "position": {"x": 512.5, "y": 640, "z": 700},
          "orientation": {"pitch": 0, "yaw": 0, "roll": 0}
        },
        "motion": {"linear": {}}
      },
      {
        "tick": 900,
        "cameraEnd": {"position": {"x": 300, "y": 480, "z": 1200.25}},
        "motion": {"spring": {"frequency": 0.5, "dampingRatio": 0.75}},
        "transition": {"type": "checkerboard", "duration": 0.25}
      },
      {
        "tick": 1500,
        "cameraStart": {"position": {"x": -10, "y": 0.5, "z": 0}},
        "cameraEnd": {"position": {"x": 20, "y": 1, "z": 40}},
        "transition": {"type": "fade", "duration": 2}
      }
    ],
    "endTick": 1800
  }
}
)"};

/// Tells whether two decimals are written alike: same mantissa and places.
///
/// @param left the first
/// @param right the second
/// @return true when identical
bool same_decimal(Decimal left, Decimal right) {
    return left.mantissa == right.mantissa && left.places == right.places;
}

/// Tells whether two cameras are identical.
///
/// @param left the first
/// @param right the second
/// @return true when identical
bool same_camera(const CameraState& left, const CameraState& right) {
    if (!same_decimal(left.position.x, right.position.x) ||
        !same_decimal(left.position.y, right.position.y) ||
        !same_decimal(left.position.z, right.position.z))
        return false;
    if (left.orientation.has_value() != right.orientation.has_value())
        return false;
    if (!left.orientation.has_value())
        return true;
    return same_decimal(left.orientation->pitch, right.orientation->pitch) &&
           same_decimal(left.orientation->yaw, right.orientation->yaw) &&
           same_decimal(left.orientation->roll, right.orientation->roll);
}

/// Tells whether two scripts are identical, ignoring where shots were read.
///
/// @param left the first
/// @param right the second
/// @return true when identical
bool same_script(const Script& left, const Script& right) {
    if (left.input.recording != right.input.recording || left.input.stage != right.input.stage ||
        !same_decimal(left.input.tickrate, right.input.tickrate))
        return false;
    if (left.output.width != right.output.width || left.output.height != right.output.height ||
        !same_decimal(left.output.framerate, right.output.framerate) ||
        left.output.chunking.mode != right.output.chunking.mode ||
        !same_decimal(left.output.chunking.length, right.output.chunking.length) ||
        left.output.show_ux != right.output.show_ux)
        return false;
    if (left.director.end_tick != right.director.end_tick ||
        left.director.shots.size() != right.director.shots.size())
        return false;
    for (size_t index{}; index < left.director.shots.size(); ++index) {
        const Shot& a{left.director.shots[index]};
        const Shot& b{right.director.shots[index]};
        if (a.tick != b.tick || a.camera_start.has_value() != b.camera_start.has_value() ||
            (a.camera_start.has_value() && !same_camera(*a.camera_start, *b.camera_start)) ||
            !same_camera(a.camera_end, b.camera_end) || a.motion.kind != b.motion.kind ||
            !same_decimal(a.motion.frequency, b.motion.frequency) ||
            !same_decimal(a.motion.damping_ratio, b.motion.damping_ratio) ||
            a.transition.has_value() != b.transition.has_value())
            return false;
        if (a.transition.has_value() &&
            (a.transition->kind != b.transition->kind ||
             !same_decimal(a.transition->duration, b.transition->duration)))
            return false;
    }
    return true;
}

/// Reads a script, expecting success and no warnings.
///
/// @param text the script's text
/// @return the script
Script read_ok(std::string_view text) {
    Script script{};
    DecodeReport report{};
    const bool read{read_script(bytes_of(text), script, report)};
    CHECK(read);
    CHECK(report.errors.empty());
    CHECK(report.warnings.empty());
    for (const Diagnostic& error : report.errors)
        std::fprintf(
            stderr,
            "  unexpected error %s at %u:%u: %s\n",
            error.path.c_str(),
            error.position.line,
            error.position.column,
            error.message.c_str()
        );
    return script;
}

/// Reads a script, expecting one error with a path, message and position.
///
/// @param text the script's text
/// @param path the key path expected
/// @param message the message expected
/// @param line the line expected; 0 for a missing key
/// @param column the column expected; 0 for a missing key
/// @param line_number the calling test's line, for the report
void expect_error(
    std::string_view text,
    std::string_view path,
    std::string_view message,
    uint32_t line,
    uint32_t column,
    int line_number
) {
    Script script{};
    DecodeReport report{};
    const bool read{read_script(bytes_of(text), script, report)};
    const bool matches{
        !read && report.errors.size() == 1 && report.errors[0].path == path &&
        report.errors[0].message == message && report.errors[0].position.line == line &&
        report.errors[0].position.column == column
    };
    oa::formats::oascript::test::check(matches, "expected one decode error", __FILE__, line_number);
    if (!matches) {
        std::fprintf(
            stderr,
            "  expected %.*s at %u:%u: %.*s\n",
            static_cast<int>(path.size()),
            path.data(),
            line,
            column,
            static_cast<int>(message.size()),
            message.data()
        );
        for (const Diagnostic& error : report.errors)
            std::fprintf(
                stderr,
                "  got %s at %u:%u: %s\n",
                error.path.c_str(),
                error.position.line,
                error.position.column,
                error.message.c_str()
            );
    }
}

/// Returns the minimal script with one line replaced.
///
/// @param from the text to replace
/// @param to its replacement
/// @return the edited script
std::string edited(std::string_view from, std::string_view to) {
    std::string text{minimal_yaml};
    const size_t at{text.find(from)};
    CHECK(at != std::string::npos);
    if (at != std::string::npos)
        text.replace(at, from.size(), to);
    return text;
}

/// Returns the minimal script with text added after the director's shots.
///
/// @param shots more shots, indented as items of director.shots
/// @return the script
std::string with_shots(std::string_view shots) {
    return std::string{minimal_yaml} + std::string{shots};
}

/// Checks the defaults of a minimal script.
void test_defaults() {
    const Script script{read_ok(minimal_yaml)};
    CHECK(script.input.recording == "game.rec");
    CHECK(same_decimal(script.input.tickrate, default_tickrate));
    CHECK(script.output.width == default_width && script.output.height == default_height);
    CHECK(same_decimal(script.output.framerate, default_framerate));
    CHECK(script.output.chunking.mode == ChunkMode::seconds);
    CHECK(same_decimal(script.output.chunking.length, default_chunk_seconds));
    CHECK(!script.output.show_ux);
    CHECK(script.director.shots.size() == 1);
    CHECK(!script.director.end_tick.has_value());
    const Shot& shot{script.director.shots[0]};
    CHECK(shot.tick == 0 && shot.camera_start.has_value() && !shot.transition.has_value());
    CHECK(shot.motion.kind == Motion::linear);
    CHECK(same_decimal(shot.motion.damping_ratio, default_damping_ratio));
    CHECK(same_decimal(shot.camera_end.position.y, Decimal{200, 0}));
    CHECK(!shot.camera_end.orientation.has_value());
    CHECK(shot.position.line == 6 && shot.position.column == 7);

    // The chunk length's default follows the mode.
    const Script ticks{
        read_ok(edited("director:", "output:\n  chunking: { mode: ticks }\ndirector:"))
    };
    CHECK(ticks.output.chunking.mode == ChunkMode::ticks);
    CHECK(same_decimal(ticks.output.chunking.length, default_chunk_ticks));
    // A spring's damping ratio defaults to 1; a transition's type to dissolve.
    const Script spring{read_ok(with_shots(R"(    - tick: 10
      cameraEnd: { position: { x: 1, y: 2, z: 3 } }
      motion: { spring: { frequency: 2 } }
      transition: { duration: 1 }
)"))};
    CHECK(spring.director.shots.size() == 2);
    const Shot& second{spring.director.shots[1]};
    CHECK(
        second.motion.kind == Motion::spring && same_decimal(second.motion.frequency, Decimal{2, 0})
    );
    CHECK(same_decimal(second.motion.damping_ratio, default_damping_ratio));
    CHECK(second.transition.has_value() && second.transition->kind == TransitionKind::dissolve);
    CHECK(!second.camera_start.has_value());
}

/// Checks a script that sets every key, read from both forms.
void test_full() {
    const Script yaml{read_ok(full_yaml)};
    const Script json{read_ok(full_json)};
    CHECK(same_script(yaml, json));
    CHECK(yaml.input.recording == "replays/game.rec");
    CHECK(same_decimal(yaml.input.tickrate, Decimal{455, 1}));
    CHECK(yaml.output.width == 3840 && yaml.output.height == 2160);
    CHECK(same_decimal(yaml.output.framerate, Decimal{5994, 2}));
    CHECK(
        yaml.output.chunking.mode == ChunkMode::ticks &&
        same_decimal(yaml.output.chunking.length, Decimal{900, 0})
    );
    CHECK(yaml.output.show_ux);
    CHECK(yaml.director.end_tick == 1800u);
    CHECK(yaml.director.shots.size() == 3);
    const Shot& first{yaml.director.shots[0]};
    CHECK(
        first.camera_start.has_value() &&
        same_decimal(first.camera_start->position.x, Decimal{1024, 0})
    );
    CHECK(same_decimal(first.camera_end.position.x, Decimal{5125, 1}));
    CHECK(first.camera_end.orientation.has_value());
    const Shot& second{yaml.director.shots[1]};
    CHECK(second.tick == 900 && !second.camera_start.has_value());
    CHECK(
        second.motion.kind == Motion::spring &&
        same_decimal(second.motion.frequency, Decimal{5, 1}) &&
        same_decimal(second.motion.damping_ratio, Decimal{75, 2})
    );
    CHECK(
        second.transition.has_value() && second.transition->kind == TransitionKind::checkerboard &&
        same_decimal(second.transition->duration, Decimal{25, 2})
    );
    const Shot& third{yaml.director.shots[2]};
    CHECK(third.transition.has_value() && third.transition->kind == TransitionKind::fade);
    CHECK(same_decimal(third.camera_start->position.x, Decimal{-10, 0}));
    CHECK(same_decimal(third.camera_start->position.y, Decimal{5, 1}));
}

/// Checks every decode error with its key path and position.
void test_errors() {
    // The top level and its keys.
    expect_error(edited("oascript: 1\n", ""), "oascript", "is required", 0, 0, __LINE__);
    expect_error(edited("oascript: 1", "oascript: 2"), "oascript", "must be 1", 1, 11, __LINE__);
    expect_error(
        edited("oascript: 1", "oascript: 1.5"),
        "oascript",
        "must be a whole number",
        1,
        11,
        __LINE__
    );
    expect_error(
        edited("oascript: 1", "oascript: '1'"), "oascript", "must be a number", 1, 11, __LINE__
    );
    expect_error(
        edited("oascript: 1", "oascript: 1\nextra: 0"),
        "extra",
        "is not a known key",
        2,
        1,
        __LINE__
    );
    expect_error(
        "oascript: 1\ninput: { demo: a }\ndirector:\n  shots: []\n",
        "director.shots",
        "must hold at least one shot",
        4,
        10,
        __LINE__
    );
    {
        Script script{};
        DecodeReport report{};
        CHECK(!read_script(bytes_of("oascript: 1\n"), script, report));
        CHECK(
            report.errors.size() == 2 && report.errors[0].path == "input" &&
            report.errors[1].path == "director"
        );
    }

    // input.
    expect_error(
        edited("  demo: game.rec\n", "  tickrate: 30\n"),
        "input.demo",
        "is required",
        0,
        0,
        __LINE__
    );
    expect_error(
        edited("demo: game.rec", "demo: ''"), "input.demo", "must not be empty", 3, 9, __LINE__
    );
    expect_error(
        edited("demo: game.rec", "demo: 5"), "input.demo", "must be a string", 3, 9, __LINE__
    );
    expect_error(
        edited("input:\n  demo: game.rec\n", "input: 3\n"),
        "input",
        "must be a mapping",
        2,
        8,
        __LINE__
    );
    expect_error(
        edited("demo: game.rec", "demo: game.rec\n  tickrate: 0"),
        "input.tickrate",
        "must be above 0 and at most 3000",
        4,
        13,
        __LINE__
    );
    expect_error(
        edited("demo: game.rec", "demo: game.rec\n  tickrate: 3000.001"),
        "input.tickrate",
        "must be above 0 and at most 3000",
        4,
        13,
        __LINE__
    );
    expect_error(
        edited("demo: game.rec", "demo: game.rec\n  tickrate: 30.0001"),
        "input.tickrate",
        "must have at most 3 decimal places",
        4,
        13,
        __LINE__
    );
    read_ok(edited("demo: game.rec", "demo: game.rec\n  tickrate: 3000.000"));
    read_ok(edited("demo: game.rec", "demo: game.rec\n  tickrate: 0.001"));
    expect_error(
        edited("demo: game.rec", "demo: game.rec\n  speed: 2"),
        "input.speed",
        "is not a known key",
        4,
        3,
        __LINE__
    );
    // A stage in place of the recording; never both.
    {
        const Script staged{read_ok(edited("demo: game.rec", "stage: battles/fight.stage"))};
        CHECK(staged.input.stage == "battles/fight.stage" && staged.input.recording.empty());
    }
    expect_error(
        edited("demo: game.rec", "demo: game.rec\n  stage: fight.stage"),
        "input.stage",
        "cannot be given with a demo: a script plays one or the other",
        4,
        10,
        __LINE__
    );
    expect_error(
        edited("demo: game.rec", "stage: ''"), "input.stage", "must not be empty", 3, 10, __LINE__
    );
    expect_error(
        edited("demo: game.rec", "stage: [a]"), "input.stage", "must be a string", 3, 10, __LINE__
    );

    // output.
    const auto output = [](std::string_view lines) {
        return edited("director:", "output:\n" + std::string{lines} + "\ndirector:");
    };
    read_ok(output("  resolution: { width: 16, height: 16384 }"));
    expect_error(
        output("  resolution: { width: 1921 }"),
        "output.resolution.width",
        "must be an even number",
        5,
        24,
        __LINE__
    );
    expect_error(
        output("  resolution: { height: 14 }"),
        "output.resolution.height",
        "must be a whole number from 16 to 16384",
        5,
        25,
        __LINE__
    );
    expect_error(
        output("  resolution: { height: 16386 }"),
        "output.resolution.height",
        "must be a whole number from 16 to 16384",
        5,
        25,
        __LINE__
    );
    expect_error(
        output("  resolution: { width: 100.5 }"),
        "output.resolution.width",
        "must be a whole number from 16 to 16384",
        5,
        24,
        __LINE__
    );
    expect_error(
        output("  resolution: { depth: 8 }"),
        "output.resolution.depth",
        "is not a known key",
        5,
        17,
        __LINE__
    );
    expect_error(
        output("  resolution: [1920, 1080]"),
        "output.resolution",
        "must be a mapping",
        5,
        15,
        __LINE__
    );
    read_ok(output("  framerate: 240"));
    expect_error(
        output("  framerate: 240.001"),
        "output.framerate",
        "must be above 0 and at most 240",
        5,
        14,
        __LINE__
    );
    expect_error(
        output("  framerate: -1"),
        "output.framerate",
        "must be above 0 and at most 240",
        5,
        14,
        __LINE__
    );
    expect_error(
        output("  framerate: 29.9701"),
        "output.framerate",
        "must have at most 3 decimal places",
        5,
        14,
        __LINE__
    );
    expect_error(
        output("  chunking: { mode: frames }"),
        "output.chunking.mode",
        "must be seconds or ticks",
        5,
        21,
        __LINE__
    );
    expect_error(
        output("  chunking: { length: 0 }"),
        "output.chunking.length",
        "must be above 0 and at most 86400",
        5,
        23,
        __LINE__
    );
    read_ok(output("  chunking: { length: 86400 }"));
    expect_error(
        output("  chunking: { length: 86400.001 }"),
        "output.chunking.length",
        "must be above 0 and at most 86400",
        5,
        23,
        __LINE__
    );
    expect_error(
        output("  chunking: { length: 0.0001 }"),
        "output.chunking.length",
        "must have at most 3 decimal places",
        5,
        23,
        __LINE__
    );
    read_ok(output("  chunking: { length: 2.5 }"));
    read_ok(output("  chunking: { mode: ticks, length: 2592000 }"));
    read_ok(output("  chunking: { length: 1800.0, mode: ticks }"));
    expect_error(
        output("  chunking: { mode: ticks, length: 2592001 }"),
        "output.chunking.length",
        "must be above 0 and at most 2592000",
        5,
        36,
        __LINE__
    );
    expect_error(
        output("  chunking: { mode: ticks, length: 1.5 }"),
        "output.chunking.length",
        "must be a whole number of ticks",
        5,
        36,
        __LINE__
    );
    expect_error(
        output("  chunking: { mode: ticks, size: 1 }"),
        "output.chunking.size",
        "is not a known key",
        5,
        28,
        __LINE__
    );
    expect_error(
        output("  showUx: yes"), "output.showUx", "must be true or false", 5, 11, __LINE__
    );
    expect_error(output("  audio: {}"), "output.audio", "is not a known key", 5, 3, __LINE__);

    // director and its shots.
    expect_error(
        "oascript: 1\ninput: { demo: a }\ndirector: {}\n",
        "director.shots",
        "is required",
        0,
        0,
        __LINE__
    );
    expect_error(
        "oascript: 1\ninput: { demo: a }\ndirector: { shots: {} }\n",
        "director.shots",
        "must be a sequence",
        3,
        20,
        __LINE__
    );
    expect_error(
        "oascript: 1\ninput: { demo: a }\ndirector: { shots: [5] }\n",
        "director.shots[0]",
        "must be a mapping",
        3,
        21,
        __LINE__
    );
    expect_error(
        edited("      cameraStart: { position: { x: 100, y: 200, z: 300 } }\n", ""),
        "director.shots[0].cameraStart",
        "is required on the first shot",
        0,
        0,
        __LINE__
    );
    expect_error(
        edited("    - tick: 0\n", "    - tick: 0\n      transition: { type: wipe, duration: 1 }\n"),
        "director.shots[0].transition",
        "is not allowed on the first shot",
        7,
        19,
        __LINE__
    );
    expect_error(
        edited("    - tick: 0\n", "    - tick: 0\n      cameraFov: 1\n"),
        "director.shots[0].cameraFov",
        "is not a known key",
        7,
        7,
        __LINE__
    );
    expect_error(
        edited("tick: 0", "tick: -1"),
        "director.shots[0].tick",
        "must be a whole number from 0 to 2147483647",
        6,
        13,
        __LINE__
    );
    expect_error(
        edited("tick: 0", "tick: 2147483648"),
        "director.shots[0].tick",
        "must be a whole number from 0 to 2147483647",
        6,
        13,
        __LINE__
    );
    read_ok(edited("tick: 0", "tick: 2147483647"));
    expect_error(
        edited("tick: 0", "tick: 0.5"),
        "director.shots[0].tick",
        "must be a whole number from 0 to 2147483647",
        6,
        13,
        __LINE__
    );
    expect_error(
        edited("      cameraEnd: { position: { x: 100, y: 200, z: 300 } }\n", ""),
        "director.shots[0].cameraEnd",
        "is required",
        0,
        0,
        __LINE__
    );
    expect_error(
        with_shots("    - cameraEnd: { position: { x: 1, y: 2, z: 3 } }\n"),
        "director.shots[1].tick",
        "is required",
        0,
        0,
        __LINE__
    );
    expect_error(
        with_shots("    - tick: 0\n      cameraEnd: { position: { x: 1, y: 2, z: 3 } }\n"),
        "director.shots[1].tick",
        "must be greater than the previous shot's tick",
        9,
        13,
        __LINE__
    );
    expect_error(
        with_shots(
            "    - tick: 5\n      cameraEnd: { position: { x: 1, y: 2, z: 3 } }\n"
            "    - tick: 5\n      cameraEnd: { position: { x: 1, y: 2, z: 3 } }\n"
        ),
        "director.shots[2].tick",
        "must be greater than the previous shot's tick",
        11,
        13,
        __LINE__
    );
    expect_error(
        with_shots("  endTick: 0\n"),
        "director.endTick",
        "must be greater than the last shot's tick",
        9,
        12,
        __LINE__
    );
    expect_error(
        with_shots("  endTick: x\n"), "director.endTick", "must be a number", 9, 12, __LINE__
    );
    expect_error(
        with_shots("  length: 5\n"), "director.length", "is not a known key", 9, 3, __LINE__
    );
    CHECK(read_ok(with_shots("  endTick: 1\n")).director.end_tick == 1u);

    // Cameras.
    expect_error(
        edited("cameraEnd: { position: { x: 100, y: 200, z: 300 } }", "cameraEnd: {}"),
        "director.shots[0].cameraEnd.position",
        "is required",
        0,
        0,
        __LINE__
    );
    expect_error(
        edited(
            "cameraEnd: { position: { x: 100, y: 200, z: 300 } }",
            "cameraEnd: { position: { x: 1, z: 3 } }"
        ),
        "director.shots[0].cameraEnd.position.y",
        "is required",
        0,
        0,
        __LINE__
    );
    expect_error(
        edited(
            "cameraEnd: { position: { x: 100, y: 200, z: 300 } }",
            "cameraEnd: { position: { y: 1, z: 3 } }"
        ),
        "director.shots[0].cameraEnd.position.x",
        "is required",
        0,
        0,
        __LINE__
    );
    expect_error(
        edited(
            "cameraEnd: { position: { x: 100, y: 200, z: 300 } }",
            "cameraEnd: { position: { x: 1, y: 3 } }"
        ),
        "director.shots[0].cameraEnd.position.z",
        "is required",
        0,
        0,
        __LINE__
    );
    expect_error(
        edited(
            "cameraEnd: { position: { x: 100, y: 200, z: 300 } }",
            "cameraEnd: { position: { x: 1, y: 0, z: 3 } }"
        ),
        "director.shots[0].cameraEnd.position.y",
        "must be above 0",
        8,
        41,
        __LINE__
    );
    expect_error(
        edited(
            "cameraEnd: { position: { x: 100, y: 200, z: 300 } }",
            "cameraEnd: { position: { x: 1, y: -0.5, z: 3 } }"
        ),
        "director.shots[0].cameraEnd.position.y",
        "must be above 0",
        8,
        41,
        __LINE__
    );
    expect_error(
        edited(
            "cameraEnd: { position: { x: 100, y: 200, z: 300 } }",
            "cameraEnd: { position: { x: a, y: 1, z: 3 } }"
        ),
        "director.shots[0].cameraEnd.position.x",
        "must be a number",
        8,
        35,
        __LINE__
    );
    expect_error(
        edited(
            "cameraEnd: { position: { x: 100, y: 200, z: 300 } }",
            "cameraEnd: { position: { x: 1, y: 1, z: 3, w: 1 } }"
        ),
        "director.shots[0].cameraEnd.position.w",
        "is not a known key",
        8,
        50,
        __LINE__
    );
    expect_error(
        edited(
            "cameraEnd: { position: { x: 100, y: 200, z: 300 } }",
            "cameraEnd: { position: { x: 1, y: 1, z: 3 }, zoom: 1 }"
        ),
        "director.shots[0].cameraEnd.zoom",
        "is not a known key",
        8,
        52,
        __LINE__
    );
    expect_error(
        edited(
            "cameraEnd: { position: { x: 100, y: 200, z: 300 } }",
            "cameraEnd: { position: { x: 1, y: 1, z: 3 }, orientation: { tilt: 0 } }"
        ),
        "director.shots[0].cameraEnd.orientation.tilt",
        "is not a known key",
        8,
        67,
        __LINE__
    );
    expect_error(
        edited(
            "cameraEnd: { position: { x: 100, y: 200, z: 300 } }",
            "cameraEnd: { position: { x: 1, y: 1, z: 3 }, orientation: { yaw: true } }"
        ),
        "director.shots[0].cameraEnd.orientation.yaw",
        "must be a number",
        8,
        72,
        __LINE__
    );

    // Motion.
    const auto motion = [](std::string_view value) {
        return edited(
            "    - tick: 0\n", "    - tick: 0\n      motion: " + std::string{value} + "\n"
        );
    };
    CHECK(read_ok(motion("{ linear: {} }")).director.shots[0].motion.kind == Motion::linear);
    expect_error(
        motion("{}"),
        "director.shots[0].motion",
        "must hold exactly one of linear and spring",
        7,
        15,
        __LINE__
    );
    expect_error(
        motion("{ linear: {}, spring: { frequency: 1 } }"),
        "director.shots[0].motion",
        "must hold exactly one of linear and spring",
        7,
        15,
        __LINE__
    );
    expect_error(
        motion("{ linear: { speed: 1 } }"),
        "director.shots[0].motion.linear",
        "must be an empty mapping",
        7,
        25,
        __LINE__
    );
    expect_error(
        motion("{ linear: }"),
        "director.shots[0].motion.linear",
        "must be an empty mapping",
        7,
        25,
        __LINE__
    );
    expect_error(
        motion("{ spring: {} }"),
        "director.shots[0].motion.spring.frequency",
        "is required",
        0,
        0,
        __LINE__
    );
    expect_error(
        motion("{ spring: { frequency: 0 } }"),
        "director.shots[0].motion.spring.frequency",
        "must be above 0 and at most 20",
        7,
        38,
        __LINE__
    );
    expect_error(
        motion("{ spring: { frequency: 20.5 } }"),
        "director.shots[0].motion.spring.frequency",
        "must be above 0 and at most 20",
        7,
        38,
        __LINE__
    );
    expect_error(
        motion("{ spring: { frequency: 1, dampingRatio: 10.01 } }"),
        "director.shots[0].motion.spring.dampingRatio",
        "must be above 0 and at most 10",
        7,
        55,
        __LINE__
    );
    expect_error(
        motion("{ spring: { frequency: 1, dampingRatio: 0 } }"),
        "director.shots[0].motion.spring.dampingRatio",
        "must be above 0 and at most 10",
        7,
        55,
        __LINE__
    );
    expect_error(
        motion("{ spring: { frequency: 1, stiffness: 2 } }"),
        "director.shots[0].motion.spring.stiffness",
        "is not a known key",
        7,
        41,
        __LINE__
    );
    expect_error(
        motion("{ linear: {}, ease: {} }"),
        "director.shots[0].motion.ease",
        "is not a known key",
        7,
        29,
        __LINE__
    );
    expect_error(
        motion("linear"), "director.shots[0].motion", "must be a mapping", 7, 15, __LINE__
    );
    read_ok(motion("{ spring: { frequency: 20, dampingRatio: 10 } }"));
    read_ok(motion("{ spring: { frequency: 0.001, dampingRatio: 0.0001 } }"));

    // Transitions.
    const auto transition = [](std::string_view value) {
        return with_shots(
            "    - tick: 5\n      cameraEnd: { position: { x: 1, y: 2, z: 3 } }\n      "
            "transition: " +
            std::string{value} + "\n"
        );
    };
    for (const std::string_view type : {"fade", "wipe", "dissolve", "checkerboard"})
        read_ok(transition("{ type: " + std::string{type} + ", duration: 60 }"));
    expect_error(
        transition("{ type: iris, duration: 1 }"),
        "director.shots[1].transition.type",
        "must be fade, wipe, dissolve or checkerboard",
        11,
        27,
        __LINE__
    );
    expect_error(
        transition("{ type: 1, duration: 1 }"),
        "director.shots[1].transition.type",
        "must be a string",
        11,
        27,
        __LINE__
    );
    expect_error(
        transition("{ type: fade }"),
        "director.shots[1].transition.duration",
        "is required",
        0,
        0,
        __LINE__
    );
    expect_error(
        transition("{ duration: 60.001 }"),
        "director.shots[1].transition.duration",
        "must be above 0 and at most 60",
        11,
        31,
        __LINE__
    );
    expect_error(
        transition("{ duration: 0 }"),
        "director.shots[1].transition.duration",
        "must be above 0 and at most 60",
        11,
        31,
        __LINE__
    );
    expect_error(
        transition("{ duration: 0.0005 }"),
        "director.shots[1].transition.duration",
        "must have at most 3 decimal places",
        11,
        31,
        __LINE__
    );
    expect_error(
        transition("{ duration: 1, easing: in }"),
        "director.shots[1].transition.easing",
        "is not a known key",
        11,
        34,
        __LINE__
    );
    expect_error(
        transition("fade"), "director.shots[1].transition", "must be a mapping", 11, 19, __LINE__
    );

    // Every error is reported, not only the first, in the order of the document.
    {
        Script script{};
        DecodeReport report{};
        CHECK(!read_script(
            bytes_of("oascript: 2\ninput: { demo: 1, tickrate: 0 }\nmore: 1\n"), script, report
        ));
        CHECK(report.errors.size() == 5);
        if (report.errors.size() == 5) {
            CHECK(report.errors[0].path == "oascript");
            CHECK(report.errors[1].path == "input.demo");
            CHECK(report.errors[2].path == "input.tickrate");
            CHECK(report.errors[3].path == "more");
            CHECK(report.errors[4].path == "director");
        }
    }
    // A read error is the only error, with an empty path.
    {
        Script script{};
        DecodeReport report{};
        CHECK(!read_script(bytes_of("oascript: 1\n  input: x\n"), script, report));
        CHECK(
            report.errors.size() == 1 && report.errors[0].path.empty() &&
            report.errors[0].position.line == 2 && report.errors[0].position.column == 3 &&
            report.errors[0].message == read_status_message(ReadStatus::bad_indentation)
        );
    }
    // A top level that is not a mapping, given to the decoder directly.
    {
        Script script{};
        DecodeReport report{};
        Node scalar{};
        scalar.kind = NodeKind::number;
        CHECK(!decode_script(scalar, script, report));
        CHECK(
            report.errors.size() == 1 && report.errors[0].path.empty() &&
            report.errors[0].message == "must be a mapping"
        );
    }
}

/// Checks the orientation warnings: a roll warns, a yaw and a pitch do not.
void test_warnings() {
    const std::string text{edited(
        "cameraEnd: { position: { x: 100, y: 200, z: 300 } }",
        "cameraEnd: { position: { x: 1, y: 1, z: 3 }, orientation: { pitch: 0, yaw: 90, "
        "roll: -0.5 } }"
    )};
    Script script{};
    DecodeReport report{};
    CHECK(read_script(bytes_of(text), script, report));
    CHECK(report.errors.empty());
    CHECK(report.warnings.size() == 1);
    if (report.warnings.size() == 1) {
        CHECK(report.warnings[0].path == "director.shots[0].cameraEnd.orientation.roll");
        CHECK(report.warnings[0].position.line == 8 && report.warnings[0].position.column == 92);
        CHECK(report.warnings[0].message == "is ignored: a camera does not roll");
    }
    CHECK(script.director.shots[0].camera_end.orientation.has_value());
    CHECK(same_decimal(script.director.shots[0].camera_end.orientation->yaw, Decimal{90, 0}));
    CHECK(same_decimal(script.director.shots[0].camera_end.orientation->roll, Decimal{-5, 1}));
    // Zero angles, however written, warn about nothing.
    Script zero{};
    DecodeReport zero_report{};
    CHECK(read_script(
        bytes_of(edited(
            "cameraEnd: { position: { x: 100, y: 200, z: 300 } }",
            "cameraEnd: { position: { x: 1, y: 1, z: 3 }, orientation: { yaw: 0.00, "
            "pitch: -0 } }"
        )),
        zero,
        zero_report
    ));
    CHECK(zero_report.warnings.empty());
}

/// Checks the encoder's key order and the writers' exact bytes.
void test_encode_golden() {
    const Script script{read_ok(full_yaml)};
    const std::string yaml{write_script(script, DocumentForm::yaml)};
    const std::string_view expected_yaml{R"(oascript: 1
input: { demo: replays/game.rec, tickrate: 45.5 }
output:
  resolution: { width: 3840, height: 2160 }
  framerate: 59.94
  chunking: { mode: ticks, length: 900 }
  showUx: true
director:
  shots:
    - tick: 0
      cameraStart:
        position: { x: 1024, y: 2048, z: 1024 }
      cameraEnd:
        position: { x: 512.5, y: 640, z: 700 }
        orientation: { pitch: 0, yaw: 0, roll: 0 }
    - tick: 900
      cameraEnd:
        position: { x: 300, y: 480, z: 1200.25 }
      motion:
        spring: { frequency: 0.5, dampingRatio: 0.75 }
      transition: { type: checkerboard, duration: 0.25 }
    - tick: 1500
      cameraStart:
        position: { x: -10, y: 0.5, z: 0 }
      cameraEnd:
        position: { x: 20, y: 1, z: 40 }
      transition: { type: fade, duration: 2 }
  endTick: 1800
)"};
    CHECK(yaml == expected_yaml);
    if (yaml != expected_yaml)
        std::fprintf(stderr, "YAML written:\n%s", yaml.c_str());
    const std::string json{write_script(script, DocumentForm::json)};
    const std::string_view expected_json{R"({
  "oascript": 1,
  "input": {"demo": "replays/game.rec", "tickrate": 45.5},
  "output": {
    "resolution": {"width": 3840, "height": 2160},
    "framerate": 59.94,
    "chunking": {"mode": "ticks", "length": 900},
    "showUx": true
  },
  "director": {
    "shots": [
      {
        "tick": 0,
        "cameraStart": {
          "position": {"x": 1024, "y": 2048, "z": 1024}
        },
        "cameraEnd": {
          "position": {"x": 512.5, "y": 640, "z": 700},
          "orientation": {"pitch": 0, "yaw": 0, "roll": 0}
        }
      },
      {
        "tick": 900,
        "cameraEnd": {
          "position": {"x": 300, "y": 480, "z": 1200.25}
        },
        "motion": {
          "spring": {"frequency": 0.5, "dampingRatio": 0.75}
        },
        "transition": {"type": "checkerboard", "duration": 0.25}
      },
      {
        "tick": 1500,
        "cameraStart": {
          "position": {"x": -10, "y": 0.5, "z": 0}
        },
        "cameraEnd": {
          "position": {"x": 20, "y": 1, "z": 40}
        },
        "transition": {"type": "fade", "duration": 2}
      }
    ],
    "endTick": 1800
  }
}
)"};
    CHECK(json == expected_json);
    if (json != expected_json)
        std::fprintf(stderr, "JSON written:\n%s", json.c_str());

    // Every output key is written, defaults included.
    const std::string minimal{write_script(read_ok(minimal_yaml), DocumentForm::yaml)};
    const std::string_view expected_minimal{R"(oascript: 1
input: { demo: game.rec, tickrate: 30 }
output:
  resolution: { width: 1920, height: 1080 }
  framerate: 60
  chunking: { mode: seconds, length: 60 }
  showUx: false
director:
  shots:
    - tick: 0
      cameraStart:
        position: { x: 100, y: 200, z: 300 }
      cameraEnd:
        position: { x: 100, y: 200, z: 300 }
)"};
    CHECK(minimal == expected_minimal);
    if (minimal != expected_minimal)
        std::fprintf(stderr, "YAML written:\n%s", minimal.c_str());
}

/// Checks decode(encode(s)) == s and the round trips through both forms.
void test_round_trips() {
    for (const std::string_view text : {minimal_yaml, full_yaml, full_json}) {
        const Script script{read_ok(text)};
        Script decoded{};
        DecodeReport report{};
        CHECK(
            decode_script(encode_script(script), decoded, report) && same_script(decoded, script)
        );
        for (const DocumentForm form : {DocumentForm::yaml, DocumentForm::json}) {
            const std::string written{write_script(script, form)};
            const Script again{read_ok(written)};
            CHECK(same_script(again, script));
            CHECK(write_script(again, form) == written);
        }
    }
    // A script built in code, with values that need care when written.
    Script script{};
    script.input.recording = "a path: with 'quotes' and \"more\"\t#1.rec";
    script.input.tickrate = Decimal{1, 3};
    script.output.framerate = Decimal{23976, 3};
    script.output.chunking = Chunking{ChunkMode::seconds, Decimal{15000, 3}};
    Shot first{};
    first.camera_start = CameraState{
        CameraPosition{Decimal{-1, 18}, Decimal{999999999999999999, 0}, Decimal{0, 4}},
        CameraOrientation{Decimal{1, 0}, Decimal{0, 0}, Decimal{-1, 0}}
    };
    first.camera_end.position = CameraPosition{Decimal{0, 0}, Decimal{1, 18}, Decimal{-5, 0}};
    first.motion = ShotMotion{Motion::spring, Decimal{20, 0}, Decimal{1, 3}};
    Shot second{};
    second.tick = max_tick;
    second.camera_end.position.y = Decimal{1, 0};
    second.transition = Transition{TransitionKind::wipe, Decimal{60000, 3}};
    script.director.shots = {first, second};
    Script decoded{};
    DecodeReport report{};
    CHECK(decode_script(encode_script(script), decoded, report) && same_script(decoded, script));
    // The roll warns; the pitch turns the camera.
    CHECK(report.warnings.size() == 1);
    for (const DocumentForm form : {DocumentForm::yaml, DocumentForm::json}) {
        Script again{};
        DecodeReport again_report{};
        CHECK(
            read_script(bytes_of(write_script(script, form)), again, again_report) &&
            same_script(again, script)
        );
    }
    // A stage is written in place of the recording and reads back.
    {
        Script staged{script};
        staged.input.recording.clear();
        staged.input.stage = "stages/fight.stage";
        Script again{};
        DecodeReport again_report{};
        const std::string text{write_script(staged, DocumentForm::yaml)};
        CHECK(text.find("stage: stages/fight.stage") != std::string::npos);
        CHECK(text.find("demo:") == std::string::npos);
        CHECK(read_script(bytes_of(text), again, again_report) && same_script(again, staged));
    }
    // The tree the encoder builds is the tree its text reads back as.
    Node read{};
    ReadError error{};
    CHECK(
        read_document(bytes_of(write_script(script, DocumentForm::yaml)), read, error) &&
        same_tree(read, encode_script(script))
    );
}

/// Feeds every truncation of a script, and damaged copies under a fixed
/// seed, to the script reader: it always answers, with an error when it fails.
void test_malformed_sweep() {
    for (const std::string_view text : {full_yaml, full_json}) {
        for (size_t length{}; length <= text.size(); ++length) {
            Script script{};
            DecodeReport report{};
            if (!read_script(bytes_of(text.substr(0, length)), script, report))
                CHECK(!report.errors.empty());
        }
    }
    uint64_t state{99};
    for (int round{}; round < 4000; ++round) {
        std::string text{round % 2 == 0 ? full_yaml : full_json};
        for (int flip{}; flip < 2; ++flip) {
            state = state * 6364136223846793005u + 1442695040888963407u;
            const size_t offset{static_cast<size_t>((state >> 33) % text.size())};
            state = state * 6364136223846793005u + 1442695040888963407u;
            text[offset] = static_cast<char>(state >> 56);
        }
        Script script{};
        DecodeReport report{};
        if (!read_script(bytes_of(text), script, report))
            CHECK(!report.errors.empty());
        else
            CHECK(report.errors.empty() && !script.director.shots.empty());
    }
}

} // namespace

int main() {
    test_defaults();
    test_full();
    test_errors();
    test_warnings();
    test_encode_golden();
    test_round_trips();
    test_malformed_sweep();
    return oa::formats::oascript::test::finish("formats-oascript-script");
}
