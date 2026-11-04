// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The platform table the Rover core runs on. oa-game selects one that finds
// ./boneyards files under the game directory; the DLL keeps the native table.

#include "platform_select.h"

namespace {

const RoverPlatform* g_platform = nullptr;

} // namespace

const RoverPlatform* rover_platform() {
    return g_platform ? g_platform : rover_native_platform();
}

void rover_use_platform(const RoverPlatform* platform) {
    g_platform = platform;
}
