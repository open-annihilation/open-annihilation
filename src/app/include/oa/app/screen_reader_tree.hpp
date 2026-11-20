// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The screen-reader tree of one control listing. A build that leaves the
// bridge out does not declare it: the nodes are AccessKit's.
#pragma once

#include "oa/platform/screen_reader.hpp"

#if !defined(OA_SCREEN_READER_COMPILED_OUT)

#include "oa/app/automation/tree.hpp"
#include "oa/platform/screen_reader/tree.hpp"

#include <cstdint>
#include <vector>

namespace oa::app::automation {
struct WindowPlacement;
}

namespace oa::app {

/// The bridge tree of one listing.
///
/// Hidden controls and decoration are left out. A tab list, list, tree or
/// radio group adopts the rows of its kind listed before it in the same
/// dialog or window, when their rectangles lie inside its own. Rows built
/// from a list's items stay that list's children. A dialog is named from an
/// alert's words or a label, and never from a button; a dialog with neither
/// is unnamed. A group is not named with an extension window's internal
/// name. Groups and dialogs take the union of their children's bounds.
///
/// The listing has no Tab index. A parent's children are ordered by place
/// when every child has one, and otherwise by position: rows of eight window
/// points, then columns. A switch's own check boxes, an empty label, and a
/// scroll bar with no label and no list beside it are left out. A scroll bar
/// with no label takes its list's name.
///
/// A focused control that is left out hands the focus to the visible button
/// whose label is that control's name or label, when no visible control is
/// focused. The main-menu overlay is left out while the screen package is
/// not the main menu.
///
/// @param listing the controls
/// @param placement where the window shows the canvas
/// @param[out] focus the focused id
/// @return the nodes a reader can reach from the window
[[nodiscard]] std::vector<oa::platform::screen_reader::Node> nodes_of(
    const oa::app::automation::ControlListing& listing,
    const oa::app::automation::WindowPlacement& placement,
    uint64_t& focus
);

} // namespace oa::app

#endif
