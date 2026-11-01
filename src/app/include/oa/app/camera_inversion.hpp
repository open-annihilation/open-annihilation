// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Which way the mouse's camera gestures go, apart from the match: Settings ›
// Controls' Invert 3D camera pan, Invert 3D camera rotation and Invert mouse
// zoom. The match reads the settings and applies the motion given here; the
// keys, the edges, touch and a controller never ask.
#pragma once

namespace oa::app::camera_inversion {

/// The gestures that go the other way, one switch each.
struct Inversion {
    bool pan{};      ///< Invert 3D camera pan: the drags that pan the turned view
    bool rotation{}; ///< Invert 3D camera rotation: the drags that orbit it
    bool zoom{};     ///< Invert mouse zoom: the wheel, in the classic view and the turned one
};

/// A motion across the screen, or the turn a drag gives: x across, y down.
struct Motion {
    float x{};
    float y{};
};

/// Returns the screen motion a pan drag of the turned view moves the ground by.
///
/// The ground under the pointer follows the pointer, so the view moves the
/// other way. With Invert 3D camera pan on the motion is turned round, and the
/// view goes the way the pointer goes. The other two switches leave it alone.
///
/// @param pointer the pointer's motion, in screen points
/// @param inversion the switches
/// @return the motion the ground makes, in screen points
[[nodiscard]] Motion pan_drag(Motion pointer, Inversion inversion) noexcept;

/// Returns the turn and the tilt an orbit drag of the turned view gives.
///
/// With Invert 3D camera rotation on both are turned round. The other two
/// switches leave them alone.
///
/// @param turn the yaw (x) and the pitch (y) the pointer's motion gives, in degrees
/// @param inversion the switches
/// @return the yaw and the pitch to orbit by, in degrees
[[nodiscard]] Motion orbit_drag(Motion turn, Inversion inversion) noexcept;

/// Returns the zoom a turn of the wheel gives, in either view.
///
/// With Invert mouse zoom on it is turned round. The other two switches leave
/// it alone.
///
/// @param amount the turn's zoom by the wheel's bindings; positive zooms in
/// @param inversion the switches
/// @return the zoom to make; positive zooms in
[[nodiscard]] float wheel_zoom(float amount, Inversion inversion) noexcept;

} // namespace oa::app::camera_inversion
