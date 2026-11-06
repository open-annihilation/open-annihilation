// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The platform table the Rover core runs on.
#pragma once

#include "oa/rover/platform.h"

/// Returns the table the Rover core runs on.
///
/// @return the table a host selected with rover_use_platform, else the native one
const RoverPlatform* rover_platform();
