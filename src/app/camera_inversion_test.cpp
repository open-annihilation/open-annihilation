// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The mouse's camera gestures under every way the three switches stand:
// Invert 3D camera pan turns a pan drag of the turned view round, across and
// down; Invert 3D camera rotation turns an orbit drag's turn and tilt round;
// Invert mouse zoom turns the wheel's zoom round, in and out, a trackpad's
// fraction of a notch included. Each leaves the other two gestures as they
// are, and with every switch off nothing changes.
#include "oa/app/camera_inversion.hpp"

#include "oa/test/check.hpp"

#include <array>

namespace {

using oa::app::camera_inversion::Inversion;
using oa::app::camera_inversion::Motion;
using oa::app::camera_inversion::orbit_drag;
using oa::app::camera_inversion::pan_drag;
using oa::app::camera_inversion::wheel_zoom;

/// Every way the switches can stand: pan, rotation and zoom.
constexpr std::array<Inversion, 8> kEveryInversion{{
    {false, false, false},
    {true, false, false},
    {false, true, false},
    {false, false, true},
    {true, true, false},
    {true, false, true},
    {false, true, true},
    {true, true, true},
}};

/// Motions left, right, up, down and across both, in points or degrees.
constexpr std::array<Motion, 5> kMotions{{
    {-12.0F, 0.0F},
    {12.0F, 0.0F},
    {0.0F, -7.5F},
    {0.0F, 7.5F},
    {3.0F, -4.0F},
}};

/// Turns of the wheel: a notch in, a notch out, a trackpad's fractions and
/// a fast spin.
constexpr std::array<float, 5> kWheelTurns{1.0F, -1.0F, 0.5F, -0.25F, 3.0F};

/// Tells whether two motions are the same.
///
/// @param left one motion
/// @param right the other
/// @return true when both parts are equal
bool same(Motion left, Motion right) {
    return left.x == right.x && left.y == right.y;
}

void every_switch_off_changes_nothing() {
    const Inversion off{};
    for (const Motion motion : kMotions) {
        OA_CHECK(same(pan_drag(motion, off), motion));
        OA_CHECK(same(orbit_drag(motion, off), motion));
    }
    for (const float turn : kWheelTurns)
        OA_CHECK(wheel_zoom(turn, off) == turn);
}

void invert_3d_camera_pan_turns_the_pan_drag_round_and_nothing_else() {
    for (const Inversion inversion : kEveryInversion)
        for (const Motion motion : kMotions) {
            const Motion ground = pan_drag(motion, inversion);
            OA_CHECK(same(ground, inversion.pan ? Motion{-motion.x, -motion.y} : motion));
        }
    // Alone, it leaves the orbit and the wheel.
    const Inversion pan{true, false, false};
    for (const Motion motion : kMotions)
        OA_CHECK(same(orbit_drag(motion, pan), motion));
    for (const float turn : kWheelTurns)
        OA_CHECK(wheel_zoom(turn, pan) == turn);
}

void invert_3d_camera_rotation_turns_the_orbit_drag_round_and_nothing_else() {
    for (const Inversion inversion : kEveryInversion)
        for (const Motion motion : kMotions) {
            const Motion turn = orbit_drag(motion, inversion);
            OA_CHECK(same(turn, inversion.rotation ? Motion{-motion.x, -motion.y} : motion));
        }
    // Alone, it leaves the pan and the wheel.
    const Inversion rotation{false, true, false};
    for (const Motion motion : kMotions)
        OA_CHECK(same(pan_drag(motion, rotation), motion));
    for (const float turn : kWheelTurns)
        OA_CHECK(wheel_zoom(turn, rotation) == turn);
}

void invert_mouse_zoom_turns_the_wheel_round_and_nothing_else() {
    for (const Inversion inversion : kEveryInversion)
        for (const float turn : kWheelTurns)
            OA_CHECK(wheel_zoom(turn, inversion) == (inversion.zoom ? -turn : turn));
    // A notch in zooms out, and a notch out zooms in.
    const Inversion zoom{false, false, true};
    OA_CHECK(wheel_zoom(1.0F, zoom) < 0.0F);
    OA_CHECK(wheel_zoom(-1.0F, zoom) > 0.0F);
    // Alone, it leaves the pan and the orbit.
    for (const Motion motion : kMotions) {
        OA_CHECK(same(pan_drag(motion, zoom), motion));
        OA_CHECK(same(orbit_drag(motion, zoom), motion));
    }
}

} // namespace

int main() {
    every_switch_off_changes_nothing();
    invert_3d_camera_pan_turns_the_pan_drag_round_and_nothing_else();
    invert_3d_camera_rotation_turns_the_orbit_drag_round_and_nothing_else();
    invert_mouse_zoom_turns_the_wheel_round_and_nothing_else();
    return oa::test::check_exit_status();
}
