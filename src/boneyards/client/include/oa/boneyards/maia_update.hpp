// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Boneyards auto-update step. Entering the Boneyards first shows the update
// console before the login console opens. Auto-update is disabled by user
// decision: the session stand-ins below report "started, nothing to update,
// completed". The console plays its own placeholder pass and does not wait
// on the idle count below.
#pragma once

#include <cstdint>

namespace oa::boneyards {

// Session start result (MaiaUpdateConsole.start_result); nonzero values are errors.
inline constexpr int32_t kMaiaStartOk = 0;
// Idle cycles the stand-in counts after the session completes (MaiaUpdateConsole.login_countdown).
inline constexpr int32_t kMaiaLoginDelay = 8;

// The fields of the update console the auto-update step uses.
struct MaiaUpdateConsole {
    int32_t login_countdown;
    int32_t start_result;
    // The session reached its completed state.
    uint8_t completed;
};

/// Constructs the update console's auto-update state.
///
/// Sets the login delay to 8 cycles, no start result and a session that has
/// not completed. The nested Maia client it also constructs is the stand-in
/// below.
///
/// @param[out] console update console whose auto-update fields are reset
void maia_update_init(MaiaUpdateConsole* console);

/// Starts the update session as the update console opens and sets the login delay.
///
/// A failed start leaves its result for maia_update_idle to refuse. A start
/// takes no command line or version text.
///
/// @param[in,out] console update console; start_result and login_countdown are written
void maia_update_open(MaiaUpdateConsole* console);

/// Starts the Maia session (stand-in).
///
/// @return kMaiaStartOk always; nonzero values would be start errors
int32_t maia_update_start();

/// Runs one step of the update state machine (stand-in).
///
/// Completes at once with no status record to report.
///
/// @param[in,out] console update console marked completed
/// @return whether a status record was produced; always false
bool maia_update_step(MaiaUpdateConsole* console);

/// Runs the update console's idle cycle (stand-in).
///
/// Steps the session until it completes, then counts the login delay down.
///
/// @param[in,out] console update console whose session and login countdown advance
/// @return true on the cycle the login console opens
bool maia_update_idle(MaiaUpdateConsole* console);

} // namespace oa::boneyards
