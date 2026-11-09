// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The touch controls' camera: the map dragged under a finger, the pinch
// zoom applied at once about the fingers, inertia and auto-scroll
// (docs/touch-controls.md).
#include "oa/app/runtime.hpp"
#include "oa/app/far_view.hpp"
#include "touch_state.hpp"
#include "oa/app/frame_pacing.hpp"
#include "oa/present/world_renderer/world_camera.hpp"
#include "oa/ui/touch_hud.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace oa::app {
namespace {

/// The time constant a pan's inertia decays with, seconds.
constexpr double kInertiaDecaySeconds = 0.25;
/// The speed under which a pan's inertia stops, points a second.
constexpr float kInertiaStopPointsPerSecond = 20.0F;

} // namespace

void Runtime::pan_match_camera_by(float dx, float dy) {
    if (screen_ != Screen::match || !selected_tnt_ || director_mode() || photo_camera_locked())
        return;
    const auto zoom = static_cast<double>(match_zoom() <= 0.0F ? 1.0F : match_zoom());
    stop_match_tracking();
    // The view's exact place moves by the finger's motion, held within the
    // view's limits as a scroll is, so that a drag past them does not have
    // to be undone before the view moves back.
    const auto view = match_view_place();
    const auto [map_width, map_height] = shown_map_size();
    const auto along =
        [&](double place, float delta, int32_t battlefield, int32_t map, double centre) {
            if (delta == 0.0F)
                return place;
            return held_view(
                place + static_cast<double>(delta) / zoom,
                static_cast<double>(battlefield) / zoom,
                static_cast<double>(map),
                past_map_edge_share(),
                view_hold_from(centre)
            );
        };
    const double x =
        along(view[0], dx, match_layout_.battlefield_width(), map_width, view_hold_.centre_x);
    const double z =
        along(view[1], dy, match_layout_.battlefield_height(), map_height, view_hold_.centre_z);
    if (x != view[0] || z != view[1])
        place_match_view(x, z);
}

void Runtime::zoom_match_about(float factor, float x, float y) {
    if (screen_ != Screen::match || !selected_tnt_ || director_mode() || photo_camera_locked() ||
        !(factor > 0.0F))
        return;
    // A zoom keeps a camera's follow of a unit: it zooms about the
    // battlefield's centre, where the follow keeps the unit, and the follow
    // goes on.
    if (match_tracking_) {
        x = static_cast<float>(match_layout_.left) +
            static_cast<float>(match_layout_.battlefield_width()) / 2.0F;
        y = static_cast<float>(match_layout_.top) +
            static_cast<float>(match_layout_.battlefield_height()) / 2.0F;
    }
    // A point off the battlefield zooms about the nearest point on it.
    const double focus_x = std::clamp(
        static_cast<double>(x) - static_cast<double>(match_layout_.left),
        0.0,
        static_cast<double>(std::max(1, match_layout_.battlefield_width()))
    );
    const double focus_y = std::clamp(
        static_cast<double>(y) - static_cast<double>(match_layout_.top),
        0.0,
        static_cast<double>(std::max(1, match_layout_.battlefield_height()))
    );
    // The zoom and its target together: nothing eases, so the map stays
    // under the fingers. Photo mode uses its own range, so a pinch does
    // not change the play zoom's settings.
    float least = least_match_zoom();
    float most = most_match_zoom();
    if (photo_mode()) {
        const std::array<float, 2> offered = photo_zoom_limits();
        least = offered[0];
        most = offered[1];
    }
    const float zoom = std::clamp(match_zoom_ * factor, least, most);
    zoom_focus_ = {false, x, y};
    zoom_view_about(zoom, focus_x, focus_y);
    match_zoom_target_ = zoom;
}

void TouchDispatchAccess::step_inertia(Runtime& runtime, uint64_t elapsed_ns) {
    auto& dispatch = runtime.touch_state().dispatch;
    // A lift in photo mode stops with the finger: the picture does not coast.
    if (runtime.photo_mode()) {
        dispatch.inertia_x = 0.0F;
        dispatch.inertia_y = 0.0F;
        return;
    }
    if (dispatch.inertia_x == 0.0F && dispatch.inertia_y == 0.0F)
        return;
    // A finger on the battlefield, another screen or a camera that cannot
    // move ends it.
    if (runtime.screen_ != Screen::match || dispatch.battlefield.fingers_down() != 0) {
        dispatch.inertia_x = 0.0F;
        dispatch.inertia_y = 0.0F;
        return;
    }
    if (elapsed_ns == 0)
        return;
    const double seconds = static_cast<double>(elapsed_ns) / 1e9;
    // The map keeps following the fingers' last motion.
    runtime.pan_match_camera_by(
        static_cast<float>(-static_cast<double>(dispatch.inertia_x) * seconds),
        static_cast<float>(-static_cast<double>(dispatch.inertia_y) * seconds)
    );
    const auto decay = static_cast<float>(std::exp(-seconds / kInertiaDecaySeconds));
    dispatch.inertia_x *= decay;
    dispatch.inertia_y *= decay;
    const float stop = points_to_pixels(runtime, kInertiaStopPointsPerSecond);
    if (std::hypot(dispatch.inertia_x, dispatch.inertia_y) < stop) {
        dispatch.inertia_x = 0.0F;
        dispatch.inertia_y = 0.0F;
    }
}

void TouchDispatchAccess::step_auto_scroll(Runtime& runtime, uint64_t elapsed_ns) {
    auto& state = runtime.touch_state();
    auto& dispatch = state.dispatch;
    dispatch.auto_scrolling = false;
    if (runtime.photo_mode())
        return;
    if (runtime.screen_ != Screen::match || !runtime.match_ ||
        !(dispatch.box_active || dispatch.ghost_drag))
        return;
    // The band lies inside the battlefield and the safe area, where a
    // finger can reach.
    const auto& layout = runtime.match_layout_;
    const int left = std::max(layout.left, layout.safe.left);
    const int top = std::max(layout.top, layout.safe.top);
    const int right =
        std::min(layout.left + layout.battlefield_width(), layout.width - layout.safe.right);
    const int bottom =
        std::min(layout.top + layout.battlefield_height(), layout.height - layout.safe.bottom);
    const float band = points_to_pixels(runtime, oa::ui::touch_hud::edge_scroll_points);
    const float x = dispatch.auto_scroll_x;
    const float y = dispatch.auto_scroll_y;
    const int way_x = x < static_cast<float>(left) + band     ? -1
                      : x >= static_cast<float>(right) - band ? 1
                                                              : 0;
    const int way_y = y < static_cast<float>(top) + band       ? -1
                      : y >= static_cast<float>(bottom) - band ? 1
                                                               : 0;
    if (way_x == 0 && way_y == 0)
        return;
    dispatch.auto_scrolling = true;
    // The keyboard's scroll rate: map pixels at zoom 1, which the pan
    // divides by the zoom as scroll_match_view does.
    const auto speed = runtime.match_->state().game.scroll_speed;
    const auto step = static_cast<float>(frame_pacing::scroll_distance(speed, elapsed_ns));
    if (step <= 0.0F)
        return;
    runtime.pan_match_camera_by(static_cast<float>(way_x) * step, static_cast<float>(way_y) * step);
    if (dispatch.box_active && runtime.match_drag_) {
        // The box's far corner is the ground now under the finger.
        runtime.match_pointer_x_ = x;
        runtime.match_pointer_y_ = y;
        runtime.track_match_drag();
    }
}

} // namespace oa::app
