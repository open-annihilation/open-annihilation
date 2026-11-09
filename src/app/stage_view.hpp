// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The presentation-only camera values of a stage's view line.
#pragma once

#include <array>
#include <cmath>
#include <istream>
#include <limits>

namespace oa::app {

/// Places a stage's view around a ground point, without changing its orders.
struct StageView {
    double centre_x{}; ///< map pixels across
    double centre_z{}; ///< map-image row, before the terrain height lift
    double zoom{};     ///< positive view scale
    double yaw{};      ///< degrees around the ground
    double pitch{};    ///< degrees above or below the ground
};

/// Reads exactly five finite camera values, leaving the output unchanged on failure.
///
/// @param[in,out] line the values after the view action
/// @param[out] view the parsed camera values
/// @return true for a complete camera line with positive zoom
inline bool read_stage_view(std::istream& line, StageView& view) {
    StageView parsed;
    if (!(line >> parsed.centre_x >> parsed.centre_z >> parsed.zoom >> parsed.yaw >> parsed.pitch))
        return false;
    line >> std::ws;
    if (!line.eof() || parsed.zoom <= 0)
        return false;
    for (const double value :
         std::array{parsed.centre_x, parsed.centre_z, parsed.zoom, parsed.yaw, parsed.pitch})
        if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
            return false;
    view = parsed;
    return true;
}

} // namespace oa::app
