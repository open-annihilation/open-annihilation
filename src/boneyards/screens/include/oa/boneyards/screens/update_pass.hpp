// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The update console's placeholder pass. The auto-updater is not run: the
// console shows these lines and a rising bar for about two seconds of the
// 30 Hz clock, then the login console opens.
#pragma once

#include <cstdint>

namespace oa::boneyards::screens {

// Frames the pass plays, one per clock tick. Sixty ticks is two seconds.
inline constexpr int32_t kUpdatePassFrames = 60;
// The status lines fill in over this many frames; the bar uses the whole pass.
inline constexpr int32_t kUpdatePassRevealFrames = 45;
inline constexpr int32_t kUpdatePassLineCount = 8;
// Thousandths of the track the bar fills, from empty to full.
inline constexpr int32_t kUpdatePassFillFull = 1000;

// What the status list says, in the order the lines appear.
inline constexpr const char* kUpdatePassLines[kUpdatePassLineCount] = {
    "Checking auto-updates...",
    "Contacting update server",
    "Reading boneyards.mmz",
    "Verifying by.ccx",
    "Verifying coreprime.ccx",
    "Verifying Boneyards\\alpha_map.gwm",
    "Verifying Boneyards\\Help",
    "No updates needed",
};

// One playing of the placeholder pass.
struct UpdatePass {
    int32_t frame; // frames already shown
    int32_t lines; // status lines visible, including the opening line
    int32_t fill;  // thousandths of the track filled from the bottom
    bool finished; // the pass has played; the login console follows
};

/// Resets a pass to its opening line, an empty bar and no frames shown.
///
/// @param[out] pass pass to reset; null does nothing
void update_pass_reset(UpdatePass* pass);

/// Shows one more frame: the bar rises, and a status line is added while the list is filling.
///
/// A finished pass stays as it is.
///
/// @param[in,out] pass pass to advance; null does nothing
void update_pass_step(UpdatePass* pass);

} // namespace oa::boneyards::screens
