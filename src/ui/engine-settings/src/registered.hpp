// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Registered settings pages: the section list that follows the engine's own
// sections, and the row-spec column a registered page shows. The dialog's
// events and its drawing both go through here.

#pragma once

#include "geometry.hpp"

#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/rows.hpp"

#include <optional>
#include <string>
#include <vector>

namespace oa::ui::engine_settings {

/// The scroll group of the section list while registered pages are listed.
/// The engine's own rows and Developer's list keep 0 and 1.
inline constexpr int32_t registered_nav_group = 3;

/// Tells whether the dialog lists registered pages in a scrolling section list.
///
/// @param dialog the dialog
/// @return true for the engine's settings once a page is registered
[[nodiscard]] bool registered_nav(const Dialog& dialog) noexcept;

/// Returns the registered page a nav control names, or -1 when it names none
/// of the pages the dialog lists.
///
/// @param dialog the dialog
/// @param control the control
/// @return the page's index, from 0
[[nodiscard]] int32_t registered_nav_index(const Dialog& dialog, int32_t control) noexcept;

/// A registered page's rows, placed and scrolled.
struct RegisteredContent {
    oa::ui::kit::PlacedRows placed{}; ///< the rows, scrolled
    oa::ui::kit::ScrollArea scroll{}; ///< where they scroll
    int32_t offset{};                 ///< how far they are scrolled
};

/// Places the open registered page's rows from their specs.
///
/// @param dialog the dialog
/// @param sizes the dialog's sizes; null uses the class's own
/// @return the rows; empty when the open page is not a registered one
[[nodiscard]] RegisteredContent
registered_content(const Dialog& dialog, const geometry::Sizes* sizes = nullptr);

/// Returns the open registered page's heading, as it was registered.
///
/// @param dialog the dialog
/// @return the title; empty when the open page is not a registered one
[[nodiscard]] std::string registered_heading(const Dialog& dialog);

/// Appends the open registered page's row parts that lie wholly in its view.
///
/// @param dialog the dialog
/// @param[in,out] parts the parts
/// @param sizes the dialog's sizes; null uses the class's own
void append_registered_content(
    const Dialog& dialog, std::vector<LayoutPart>& parts, const geometry::Sizes* sizes = nullptr
);

/// Presses a registered row with Space, or shows the registered page a nav
/// entry names. Other controls are left to the dialog.
///
/// @param[in,out] dialog the dialog
/// @param control the control
/// @return what it did, or nothing when the control is not one of these
[[nodiscard]] std::optional<DialogAction> activate_registered(Dialog& dialog, int32_t control);

/// Presses a registered row where the pointer came up.
///
/// @param[in,out] dialog the dialog
/// @param control the control
/// @param point the pointer, in source pixels
/// @return what it did, or nothing when the control is not a registered row
[[nodiscard]] std::optional<DialogAction>
press_registered_row(Dialog& dialog, int32_t control, oa::ui::kit::Point point);

/// Steps a registered row with Left or Right.
///
/// @param[in,out] dialog the dialog
/// @param control the control
/// @param up true for Right
/// @return what it did, or nothing when the control is not a registered row
[[nodiscard]] std::optional<DialogAction>
step_registered_row(Dialog& dialog, int32_t control, bool up);

/// Scrolls the open registered page when it is taller than its view.
///
/// @param[in,out] dialog the dialog
/// @param notches the wheel's turn
/// @return what the page did, or nothing when it does not scroll
[[nodiscard]] std::optional<DialogAction> wheel_registered_content(Dialog& dialog, float notches);

/// Puts the open registered page's keys back to each row's default text.
///
/// @param[in,out] dialog the dialog
void restore_registered_page(Dialog& dialog);

} // namespace oa::ui::engine_settings
