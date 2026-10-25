// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The director's camera: views of the battlefield, their limits on a map,
// the engine camera that draws a view, and a script's shots compiled into
// the camera of every video frame.
//
// A view is a centre on the ground plane and the height of the map it
// shows, in map pixels (oascript.hpp). It is drawn at zoom = output height /
// view height, and it never shows past the map: its height lies between
// min_view_height and the height at which the whole map fits the output
// (fit_height), and its centre keeps it within the map's bounds, the game's
// Game.map_pixel_width by Game.map_pixel_height.
//
// Shot s runs from its tick to the next shot's tick (the last to the end
// tick); its frames are those that show its ticks, and game time is never
// skipped. Each shot moves five quantities from a start to an end: the
// centre's x and z, 1 / height, in which zoom changes evenly, and the
// camera's yaw and pitch, in degrees. Its start
// is its cameraStart, or for a shot without one the state the shot before
// reaches at its end, its speed included. With frames k = 0 .. n - 1 and
// the end state at k = n:
//
//   linear  value(k) = start + (end - start) k / n, at the constant speed
//           (end - start) framerate / n
//   spring  value(0) = start, speed 0 (or the carried speed); each later
//           frame first sets speed += dt (w^2 (end - value) - 2 d w speed)
//           and then value += dt speed, with dt = 1 / framerate,
//           w = 2 pi frequency and d the damping ratio
//
// A transition of m frames covers the first m frames of its shot, which draw
// the tick twice: once from the shot's own camera and once from the view the
// shot before reached at its end, held still (transition.hpp blends them).
// Sounds are always placed by the shot's own camera.
//
// The views every frame shows are computed with addition, subtraction,
// multiplication, division and square root of doubles, conversions and
// floor, in the order written here and nothing else, so that every
// platform computes the same frames. Clamping to the map applies to each
// frame's view, never to the moving state.
#pragma once

#include "oa/formats/oascript.hpp"
#include "oa/media/director/clock.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace oa::media::director {

/// The smallest view height, in map pixels: 8 output pixels a map pixel at
/// 2160 lines, 4 at 1080.
inline constexpr double min_view_height = 270.0;
/// The elevation the classic view looks down at, in degrees: the angle
/// whose tangent is 2, at which a point's height lifts it half as far.
inline constexpr double classic_elevation = 63.43494882292201;
/// The lowest and highest elevation a turned camera looks down at, in
/// degrees: the game's range with Unlocked camera on.
inline constexpr double min_elevation = 10.0;
inline constexpr double max_elevation = 85.0;
/// The pitch a camera is held within, in degrees from the classic view's
/// elevation: min_elevation and max_elevation less classic_elevation.
inline constexpr double min_pitch = min_elevation - classic_elevation;
inline constexpr double max_pitch = max_elevation - classic_elevation;
/// Pi, as the double nearest to it; a spring's frequency times 2 pi is its
/// angular frequency.
inline constexpr double pi = 3.141592653589793;
/// The largest step a spring may take: both its angular frequency times dt
/// (a = 2 pi frequency / framerate) and its damping ratio times that (d a).
/// Within it the stepping settles, and at a damping ratio of 1 or more it
/// never swings from one frame to the next (a (a + 4 d) stays below 4 and
/// 2 d a at most 1).
inline constexpr double max_spring_step = 0.5;

/// The size of the video's frames, in pixels.
struct OutputSize {
    uint32_t width{};
    uint32_t height{};
};

/// The part of the map a view may show, in map pixels from the top-left
/// corner: Game.map_pixel_width by Game.map_pixel_height.
struct MapBounds {
    int32_t width{};
    int32_t height{};
};

/// A view of the battlefield, in map pixels, and the camera's turn.
struct View {
    double x{};      ///< the centre's column
    double z{};      ///< the centre's map-image row: a world point's z less half its height
    double height{}; ///< the height of the map shown
    /// Degrees turned about the vertical from the classic view, about the
    /// ground at the centre; above 0 turns the picture clockwise.
    double yaw{};
    /// Degrees of elevation above the classic view's; below 0 looks lower.
    double pitch{};
};

/// Tells whether a view turns the camera from the classic view.
///
/// @param view the view
/// @return true when its yaw or its pitch is not 0
[[nodiscard]] constexpr bool turned(const View& view) noexcept {
    return view.yaw != 0.0 || view.pitch != 0.0;
}

/// Returns the height of the view that fits the whole map in the output.
///
/// @param bounds the map's bounds
/// @param output the output size
/// @return min(bounds.height, bounds.width * output.height / output.width),
///         at least min_view_height
[[nodiscard]] double fit_height(MapBounds bounds, OutputSize output) noexcept;

/// Returns a view kept on the map.
///
/// The height is clamped to [min_view_height, fit_height], then the centre
/// to [w / 2, bounds.width - w / 2] and [height / 2, bounds.height - height / 2],
/// w being the view's width, height * output.width / output.height; an axis
/// on which the view is as large as the map centres on it. The pitch is
/// clamped to [min_pitch, max_pitch]; the yaw is kept as it is. The centre
/// is held as for a camera that does not turn, so a turned view may show
/// past the map's edges.
///
/// @param view the view
/// @param bounds the map's bounds
/// @param output the output size
/// @return the clamped view
[[nodiscard]] View clamp_view(View view, MapBounds bounds, OutputSize output) noexcept;

/// Returns the view a script's camera names.
///
/// @param camera the camera; its orientation's roll is ignored
/// @return its x, z, y (as height), yaw and pitch, each its decimal mantissa
///         divided by 10 to the power of its places; yaw and pitch 0 without
///         an orientation
[[nodiscard]] View view_of_camera(const oa::formats::oascript::CameraState& camera) noexcept;

/// The engine camera that draws a view at the output size.
///
/// The engine draws from a whole map pixel. To place the view to a fraction
/// of one, the frame is drawn `margin` output pixels wider and taller from
/// (left, top) and cut out from (offset_x, offset_y).
struct EngineView {
    int32_t left{};           ///< floor of the view's left edge, map pixels
    int32_t top{};            ///< floor of its top edge, map-image rows
    int32_t visible_width{};  ///< map pixels across the view, rounded to nearest
    int32_t visible_height{}; ///< map pixels down the view, rounded to nearest
    double zoom{};            ///< output pixels a map pixel
    uint32_t offset_x{};      ///< floor((left edge - left) * zoom), output pixels
    uint32_t offset_y{};      ///< floor((top edge - top) * zoom), output pixels
    uint32_t margin{};        ///< ceil(zoom), output pixels drawn past the right and bottom
};

/// Returns the engine camera that draws a view.
///
/// zoom = output.height / height; the view's width is height *
/// output.width / output.height; its left edge is x - width / 2 and its top
/// edge z - height / 2.
///
/// @param view a view clamp_view returned
/// @param output the output size
/// @return the engine camera
[[nodiscard]] EngineView engine_view(View view, OutputSize output) noexcept;

/// One shot, compiled to frames.
struct CompiledShot {
    uint32_t tick{};        ///< the tick it starts on
    uint64_t first_frame{}; ///< the first frame that shows its tick
    uint64_t frame_count{}; ///< its frames, at least 1
    bool continues{};       ///< no cameraStart: it starts where the shot before ends
    View start{};           ///< cameraStart, when it does not continue
    View end{};             ///< cameraEnd
    oa::formats::oascript::Motion motion{oa::formats::oascript::Motion::linear};
    double frequency{};     ///< a spring's, in hertz
    double damping_ratio{}; ///< a spring's
    oa::formats::oascript::TransitionKind transition{
        oa::formats::oascript::TransitionKind::dissolve
    };
    uint64_t transition_frames{}; ///< 0 for none; at most frame_count
    size_t script_index{};        ///< the shot's index in director.shots
};

/// A script's shots on the frame clock.
struct ShotList {
    FrameClock clock{};
    ChunkRule chunks{};
    OutputSize output{};
    MapBounds bounds{};
    uint32_t end_tick{};    ///< the first tick no frame shows
    uint64_t frame_count{}; ///< the video's frames
    std::vector<CompiledShot> shots{};
};

/// What compiling a script found, each message starting with its key path.
struct CompileMessages {
    std::vector<std::string> errors{};
    std::vector<std::string> warnings{};
};

/// Compiles a decoded script's shots for a map.
///
/// The video ends before director.endTick, or when it is absent before
/// `recording_end_tick`. Errors: an end tick not after the last shot's tick,
/// a video of more than max_frame_count frames, a shot that no frame shows,
/// a transition longer than its shot or of no frame, a spring whose
/// a = 2 pi frequency / framerate, or whose damping ratio times a, exceeds
/// max_spring_step. Warnings: a camera clamp_view moves, a view height
/// outside [min_view_height, fit_height], and a pitch outside [min_pitch,
/// max_pitch].
///
/// @param script a script decode_script accepted
/// @param bounds the map's bounds
/// @param recording_end_tick the tick after the recording's last, used when
///        the script has no endTick
/// @param[out] shots the compiled shots; complete only when the call succeeds
/// @param[out] messages the errors and warnings
/// @return true when there was no error
[[nodiscard]] bool compile_shots(
    const oa::formats::oascript::Script& script,
    MapBounds bounds,
    uint32_t recording_end_tick,
    ShotList& shots,
    CompileMessages& messages
);

/// The cameras of one video frame.
struct FrameCameras {
    uint64_t frame{};
    uint32_t tick{};   ///< the tick the frame shows
    size_t shot{};     ///< the index of its shot in ShotList::shots
    View camera{};     ///< the shot's own view, clamped; sounds are placed by its x and z
    bool transition{}; ///< the frame blends `outgoing` into `camera`
    View outgoing{};   ///< the view the shot before reached at its end, clamped
    oa::formats::oascript::TransitionKind transition_kind{
        oa::formats::oascript::TransitionKind::dissolve
    };
    uint32_t transition_step{};  ///< 0 to transition_steps - 1
    uint32_t transition_steps{}; ///< the transition's frames
};

/// Steps a compiled script's cameras one frame at a time, in order.
///
/// Springs are stepped, not solved, so the frames must be taken in order
/// from frame 0; a rig cannot seek.
class CameraRig {
  public:

    /// Starts at frame 0.
    ///
    /// @param shots the compiled shots; must outlive the rig
    explicit CameraRig(const ShotList& shots);

    /// Returns the next frame's cameras.
    ///
    /// @param[out] cameras the cameras of frame position(); unchanged at the end
    /// @return false once every frame has been returned
    [[nodiscard]] bool next(FrameCameras& cameras);

    /// Returns the frame next() returns next.
    ///
    /// @return the frame, counted from 0
    [[nodiscard]] uint64_t position() const noexcept;

  private:

    /// One moving quantity: a value and its speed, per second.
    struct Axis {
        double value{};
        double speed{};
    };

    const ShotList* shots_{};
    uint64_t frame_{};
    size_t shot_{};
    Axis x_{};              ///< the centre's column
    Axis z_{};              ///< the centre's row
    Axis inverse_height_{}; ///< 1 / the view's height
    Axis yaw_{};            ///< the camera's yaw, degrees
    Axis pitch_{};          ///< the camera's pitch, degrees
    View held_{};           ///< the view the shot before reached at its end
};

} // namespace oa::media::director
