// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The way of the mouse's camera gestures, with no match and no window: each
// switch turns its own gestures round and leaves the others as they are.
#include "oa/app/camera_inversion.hpp"

namespace oa::app::camera_inversion {

Motion pan_drag(Motion pointer, Inversion inversion) noexcept {
    if (!inversion.pan)
        return pointer;
    return {-pointer.x, -pointer.y};
}

Motion orbit_drag(Motion turn, Inversion inversion) noexcept {
    if (!inversion.rotation)
        return turn;
    return {-turn.x, -turn.y};
}

float wheel_zoom(float amount, Inversion inversion) noexcept {
    return inversion.zoom ? -amount : amount;
}

} // namespace oa::app::camera_inversion
