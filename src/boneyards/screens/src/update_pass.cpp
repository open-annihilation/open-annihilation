// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Frame steps of the update console's placeholder pass. Nothing is read or
// downloaded: the lines and the bar are the whole of the pass.
#include "oa/boneyards/screens/update_pass.hpp"

namespace oa::boneyards::screens {

void update_pass_reset(UpdatePass* pass) {
    if (pass == nullptr)
        return;
    *pass = {};
    pass->lines = 1;
}

void update_pass_step(UpdatePass* pass) {
    if (pass == nullptr || pass->finished)
        return;
    const int32_t shown = pass->frame;
    if (shown >= kUpdatePassRevealFrames - 1)
        pass->lines = kUpdatePassLineCount;
    else
        pass->lines = 1 + shown * (kUpdatePassLineCount - 1) / (kUpdatePassRevealFrames - 1);
    pass->fill = (shown + 1) * kUpdatePassFillFull / kUpdatePassFrames;
    pass->frame = shown + 1;
    pass->finished = pass->frame >= kUpdatePassFrames;
}

} // namespace oa::boneyards::screens
