// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A director script (.oascript): the recording to replay, or the stage to
// play, the video to make of it and the camera's shots, read from and
// written as the document tree of oascript/document.hpp. The keys, their
// defaults and their meaning:
//
//   oascript: 1                 the script format's version; required
//   input:
//     demo: PATH                the recording an extension of this build
//                               replays, absolute or relative to the
//                               script's folder (or its name inside a
//                               .oamovie bundle); required unless stage
//                               is given
//     stage: PATH               a stage file the game plays out over a
//                               skirmish in place of a recording, absolute
//                               or relative to the script's folder; a
//                               script names a recording or a stage, not
//                               both
//     tickrate: 30              game ticks a second of video shows
//   output:
//     resolution: { width: 1920, height: 1080 }   even, in pixels
//     framerate: 60             frames a second of video
//     chunking: { mode: seconds, length: 60 }     or mode: ticks, length: 1800
//     showUx: false             draw the game's interface over the battlefield
//   director:
//     shots:                    at least one, in increasing tick order
//       - tick: 0               the game tick the shot starts on
//         cameraStart: CAMERA   the camera at the start; omitted, the shot
//                               continues from the previous shot's camera
//         cameraEnd: CAMERA     the camera at the end; required
//         motion: { linear: {} } or { spring: { frequency: HZ, dampingRatio: 1.0 } }
//         transition: { type: fade|wipe|dissolve|checkerboard, duration: SECONDS }
//     endTick: TICK             the tick the video ends on (exclusive)
//
//   CAMERA: { position: { x: X, y: Y, z: Z }, orientation: { pitch, yaw, roll } }
//
// x and z are the centre of the view in map pixels on the ground plane (z is
// the map-image row, a world point's z less half its height); y is the height
// of the map the view shows, in map pixels. orientation turns the camera, in
// degrees: yaw about the vertical from the classic view, pitch in elevation
// from the classic view's; both 0 when it is left out. A camera does not
// roll: roll is read and ignored, and one that is not zero gives a warning.
//
// decode_script checks what can be checked without the map or the
// recording: types, ranges, required keys, unknown keys, shot order and the
// first shot's camera. Each error and warning names its key path, such as
// director.shots[2].cameraEnd.position.y, and the position of its value.
#pragma once

#include "oa/formats/oascript/document.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace oa::formats::oascript {

/// The script format's version, the value of the oascript key.
inline constexpr uint32_t script_version = 1;
/// The default output width and height, in pixels.
inline constexpr uint32_t default_width = 1920;
inline constexpr uint32_t default_height = 1080;
/// The narrowest and widest output side, in pixels.
inline constexpr uint32_t min_dimension = 16;
inline constexpr uint32_t max_dimension = 16384;
/// The default frame rate, frames a second of video.
inline constexpr Decimal default_framerate{60, 0};
/// The highest frame rate.
inline constexpr Decimal max_framerate{240, 0};
/// The default tick rate: game ticks a second of video, the game's normal speed.
inline constexpr Decimal default_tickrate{30, 0};
/// The highest tick rate, a hundred times the game's normal speed.
inline constexpr Decimal max_tickrate{3000, 0};
/// The most decimal places a rate, a chunk length or a transition's
/// duration may have, so that the frame clock's products stay within 63
/// bits.
inline constexpr uint32_t max_rate_places = 3;
/// The default chunk length in each chunking mode.
inline constexpr Decimal default_chunk_seconds{60, 0};
inline constexpr Decimal default_chunk_ticks{1800, 0};
/// The longest chunk in each chunking mode: a day of video, or a day of game
/// time at the game's normal speed.
inline constexpr Decimal max_chunk_seconds{86400, 0};
inline constexpr Decimal max_chunk_ticks{2592000, 0};
/// The longest transition, in seconds.
inline constexpr Decimal max_transition_seconds{60, 0};
/// The highest spring frequency, in hertz.
inline constexpr Decimal max_spring_frequency{20, 0};
/// The default and highest spring damping ratio; 1 settles without overshoot.
inline constexpr Decimal default_damping_ratio{1, 0};
inline constexpr Decimal max_damping_ratio{10, 0};
/// The most shots a script holds.
inline constexpr size_t max_shot_count = 100000;
/// The largest tick a script names.
inline constexpr uint32_t max_tick = 0x7fffffff;

/// The centre and height of a view, in map pixels (see the file comment).
struct CameraPosition {
    Decimal x{};
    Decimal y{}; ///< the height of the map the view shows; above zero
    Decimal z{};
};

/// A camera's turn, in degrees.
struct CameraOrientation {
    Decimal pitch{}; ///< elevation above the classic view's; below 0 is lower
    Decimal
        yaw{}; ///< turn about the vertical from the classic view; above 0 turns the picture clockwise
    Decimal roll{}; ///< read and ignored
};

/// A camera: cameraStart and cameraEnd.
struct CameraState {
    CameraPosition position{};
    std::optional<CameraOrientation> orientation{};
};

/// How a shot's camera moves from its start to its end.
enum class Motion : uint8_t {
    linear, ///< constant speed over the ground, even change of 1 / y
    spring, ///< a damped spring toward the end, stepped once a frame
};

/// A shot's motion key.
struct ShotMotion {
    Motion kind{Motion::linear};
    Decimal frequency{};                          ///< a spring's, in hertz; above zero
    Decimal damping_ratio{default_damping_ratio}; ///< a spring's; above zero
};

/// How a shot's first frames take over from the shot before.
enum class TransitionKind : uint8_t {
    fade,         ///< the outgoing view fades to black, then the incoming one from black
    wipe,         ///< the incoming view slides in from the left edge
    dissolve,     ///< the outgoing view blends into the incoming one
    checkerboard, ///< the squares of a checkerboard fill with the incoming view
};

/// A shot's transition key.
struct Transition {
    TransitionKind kind{TransitionKind::dissolve};
    Decimal duration{}; ///< seconds, above zero; rounded to whole frames
};

/// One shot of director.shots.
struct Shot {
    uint32_t tick{};
    std::optional<CameraState> camera_start{}; ///< cameraStart; none continues
    CameraState camera_end{};                  ///< cameraEnd
    ShotMotion motion{};
    std::optional<Transition> transition{};
    TextPosition position{}; ///< where the shot's mapping starts, for later messages
};

/// The director key.
struct Director {
    std::vector<Shot> shots{};
    std::optional<uint32_t> end_tick{}; ///< endTick; none ends with the recording
};

/// How the video is cut into files.
enum class ChunkMode : uint8_t {
    seconds, ///< every `length` seconds of video
    ticks,   ///< every `length` game ticks
};

/// output.chunking.
struct Chunking {
    ChunkMode mode{ChunkMode::seconds};
    Decimal length{default_chunk_seconds}; ///< above zero; the mode's default when omitted
};

/// The output key.
struct Output {
    uint32_t width{default_width};   ///< resolution.width
    uint32_t height{default_height}; ///< resolution.height
    Decimal framerate{default_framerate};
    Chunking chunking{};
    bool show_ux{}; ///< showUx
};

/// The input key.
struct Input {
    std::string recording{}; ///< the demo key: the recording's path or bundle entry name
    std::string stage{};     ///< the stage key: the stage file's path; empty for a recording
    Decimal tickrate{default_tickrate};
};

/// A whole director script.
struct Script {
    Input input{};
    Output output{};
    Director director{};
};

/// One error or warning of the decoder.
struct Diagnostic {
    std::string path{};      ///< the key path, such as director.shots[2].tick
    TextPosition position{}; ///< where that value starts; zero when the key is missing
    std::string message{};   ///< what is wrong, such as "must be an even number"
};

/// Everything the decoder found.
struct DecodeReport {
    std::vector<Diagnostic> errors{};
    std::vector<Diagnostic> warnings{};
};

/// Decodes a document tree into a script.
///
/// Every key is checked and every error is reported, not only the first.
/// Omitted optional keys take their defaults. Unknown keys are errors. A
/// non-zero orientation roll is a warning.
///
/// @param root the document's top-level mapping
/// @param[out] script the script; complete only when the call succeeds
/// @param[out] report the errors and warnings, in document order
/// @return true when there was no error
[[nodiscard]] bool decode_script(const Node& root, Script& script, DecodeReport& report);

/// Reads a script's text: read_document, then decode_script.
///
/// A read error becomes the report's only error, with an empty key path.
///
/// @param text the script's bytes, at most max_input_bytes
/// @param[out] script the script; complete only when the call succeeds
/// @param[out] report the errors and warnings
/// @return true when there was no error
[[nodiscard]] bool read_script(std::span<const uint8_t> text, Script& script, DecodeReport& report);

/// Builds the document tree of a script.
///
/// Keys come in the order the file comment lists them. Every output key is
/// written; the stage in place of the demo when the script names one, a
/// shot's motion only when it is a spring, its cameraStart and transition
/// only when present, a camera's orientation only when present, and endTick
/// only when set.
///
/// @param script the script
/// @return its tree, which decode_script reads back into the same script
[[nodiscard]] Node encode_script(const Script& script);

/// Writes a script as text: encode_script, then write_document.
///
/// @param script the script
/// @param form the form to write
/// @return the text
[[nodiscard]] std::string write_script(const Script& script, DocumentForm form);

} // namespace oa::formats::oascript
