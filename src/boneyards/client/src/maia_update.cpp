// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Auto-update stand-ins: the Maia step always reports nothing to update.
#include "oa/boneyards/maia_update.hpp"

namespace oa::boneyards {

void maia_update_init(MaiaUpdateConsole* console) {
    *console = MaiaUpdateConsole{};
    console->login_countdown = kMaiaLoginDelay;
}

void maia_update_open(MaiaUpdateConsole* console) {
    console->start_result = maia_update_start();
    console->login_countdown = kMaiaLoginDelay;
}

int32_t maia_update_start() {
    return kMaiaStartOk;
}

bool maia_update_step(MaiaUpdateConsole* console) {
    console->completed = 1;
    return false;
}

bool maia_update_idle(MaiaUpdateConsole* console) {
    // The stand-in step never produces a status record to drain.
    if (console->completed == 0)
        (void)maia_update_step(console);
    if (console->start_result != kMaiaStartOk || console->completed == 0)
        return false;
    return --console->login_countdown == 0;
}

} // namespace oa::boneyards
