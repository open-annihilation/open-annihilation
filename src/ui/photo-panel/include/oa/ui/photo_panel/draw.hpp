// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Drawing the photo panel, its guides and its toast.
#pragma once

#include "oa/ui/kit/canvas.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/photo_panel/layout.hpp"

namespace oa::ui::photo_panel {

/// Draws the guides, the chips, the panel and the toast.
///
/// Guides are drawn on this layer only. The focus ring is drawn when the
/// kit would draw it. `list` is the layout's list, with the ring added
/// when it shows.
///
/// @param canvas where it is drawn
/// @param model the panel
/// @param placed the layout
/// @param list the display list, including the ring when it shows
void draw_panel(
    const kit::Canvas& canvas,
    const Model& model,
    const Layout& placed,
    const kit::DisplayList& list
);

/// The layout's display list, with the focus ring appended when it shows.
///
/// @param placed the layout
/// @param interaction the focus
/// @return the list a screen gives automation, and draws
[[nodiscard]] kit::DisplayList
shown_list(const Layout& placed, const kit::Interaction& interaction);

} // namespace oa::ui::photo_panel
