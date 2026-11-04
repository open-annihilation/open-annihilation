// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// What the MCP server reads of the screen shown: the same fields the
// automation endpoint's screen answer gives, the control tree beside the
// flat list, and the point a named control is clicked at. None of this is
// written into a --fark answer.
#pragma once

#include "oa/app/automation/tree.hpp"
#include "oa/app/automation_host.hpp"
#include "oa/app/check_host.hpp"

#include "events.hpp"
#include "input_events.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app::automation {

/// The point a control is pressed at, in the space the input events use.
struct ClickPoint {
    int32_t x{};        ///< the column
    int32_t y{};        ///< the row
    PointSpace space{}; ///< game or window
};

/// Reads control text the way the MCP server reports it.
///
/// A control's text, each list row and each row's key are game text: bytes
/// of the game's code page, or UTF-8 where a profile already sent it. Each
/// becomes UTF-8. A masked field's text is left as the game holds it; the
/// tree still writes that field empty. The gadgets themselves are not changed.
///
/// @param[in,out] controls the controls collect_controls returned
void read_control_text(std::vector<AutomationControl>& controls);

/// Collects the flat list and the tree of the screen shown.
///
/// Text, list rows and row keys are read with read_control_text.
///
/// @param check the check host
/// @param automation the automation host
/// @return the listing; the tree's roots are the extension windows, then the
///         screen package
[[nodiscard]] ControlListing listing_of(const CheckHost& check, const AutomationHost& automation);

/// The screen answer's fields, as one JSON object.
///
/// screen, frontend_state, dialogs, focused, pointer, camera (null outside
/// a match) and hovered.
///
/// @param check the check host
/// @param automation the automation host
/// @return the object
[[nodiscard]] std::string screen_object(const CheckHost& check, const AutomationHost& automation);

/// The control tree, as one JSON object.
///
/// screen, and roots: each group has group, kind, name and children; each
/// control has the flat list's fields, plus parent, parent_kind and hovered.
///
/// @param check the check host
/// @param automation the automation host
/// @return the object
[[nodiscard]] std::string tree_object(const CheckHost& check, const AutomationHost& automation);

/// Finds a control by name.
///
/// The spelling the listing gives is tried first. Failing that, the name
/// compared ignoring ASCII case.
///
/// @param listing the listing the pointer refers into; it must outlive the pointer
/// @param wanted the name
/// @return the control, or null when the listing has none of that name
[[nodiscard]] const ListedControl*
find_named(const ControlListing& listing, std::string_view wanted);

/// The centre of a control, where a click lands.
///
/// A control whose window rectangle is known exactly is clicked in the
/// window's pixels. Any other is clicked at the centre of its canvas
/// rectangle. A control with no area has no centre.
///
/// @param control the control
/// @return the point, or nothing when the control has no area
[[nodiscard]] std::optional<ClickPoint> control_centre(const AutomationControl& control);

/// The centre of one row of a list, in the same space as the list's centre.
///
/// The row must be on the page the list is showing. A list drawn in the
/// window's pixels maps the row's place in the canvas rectangle through
/// the window rectangle.
///
/// @param row_list the list
/// @param row the row's index in its items
/// @return the point, or nothing when the row is off the page or the list
///         has no row pitch
[[nodiscard]] std::optional<ClickPoint>
list_row_point(const AutomationControl& row_list, int32_t row);

/// Reads what the event watch compares, for one moment.
///
/// @param check the check host
/// @param automation the automation host
/// @param[out] now replaced by this moment
/// @param dialogs also read the dialogs over the screen
void fill_observed(
    const CheckHost& check, const AutomationHost& automation, Observed& now, bool dialogs
);

} // namespace oa::app::automation
