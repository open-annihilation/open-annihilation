// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Pointer and key input on the photo panel.
#pragma once

#include "oa/ui/kit/input.hpp"
#include "oa/ui/photo_panel/layout.hpp"
#include "oa/ui/screen_registry.hpp"

namespace oa::ui::photo_panel {

/// What one pointer or key event asks the host to do.
struct Answer {
    bool consumed{false};      ///< true when the battlefield must not also take it
    bool redraw{false};        ///< true when what the panel draws changed
    bool leave{false};         ///< true when photo mode closes
    bool dismiss_toast{false}; ///< true when this press hides the photo notice
    Press press{};             ///< the command, when there is one
};

/// Applies a pointer event.
///
/// A point on the panel, the chips, the key line, the hint or the toast is
/// consumed, so a battlefield click there does not pan. A point on the
/// battlefield is not consumed. The ring hides when the pointer moves, as
/// the kit does.
///
/// @param[in,out] interaction the focus and the pointer
/// @param model the panel
/// @param placed the layout
/// @param kind which pointer event
/// @param at the point, in the screen's points
/// @return what the host does
[[nodiscard]] Answer pointer_event(
    kit::Interaction& interaction,
    const Model& model,
    const Layout& placed,
    oa::app::ScreenInputKind kind,
    kit::Point at
);

/// Applies a key the kit knows.
///
/// The ring stays hidden until Tab or Shift+Tab, and a pointer move hides it
/// again. While it is hidden, arrows and Space are not consumed, so they pan
/// and freeze as they do with the panel closed to the keys. Return is not
/// consumed then either, so it takes a photo. While the ring shows, the
/// kit's keys move the focus and press the focused control, and Return or a
/// pointer move hides the ring.
///
/// @param[in,out] interaction the focus
/// @param model the panel
/// @param placed the layout
/// @param key the key
/// @return what the host does
[[nodiscard]] Answer
key_event(kit::Interaction& interaction, const Model& model, const Layout& placed, kit::Key key);

} // namespace oa::ui::photo_panel
