// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// What a press, a drag or a key does to Developer Mode's list of the
// standard hacks (developer.cpp): opening and closing its areas and hacks,
// and turning hacks on and off and setting their parameters, which changes
// the chosen settings' overrides. The dialog's events (dialog.cpp) call
// these for the list's rows; the list is placed by geometry::place_list.
#pragma once

#include "oa/ui/engine_settings/dialog.hpp"

#include "geometry.hpp"

#include <cstdint>

namespace oa::ui::engine_settings::developer {

/// Opens or closes an area or a hack, as Space or a press on its header does.
///
/// @param[in,out] dialog the dialog
/// @param row an area's or a hack's header; any other row is left alone
/// @return DialogAction::redraw when it opened or closed, else DialogAction::none
DialogAction toggle_open(Dialog& dialog, const geometry::ListRow& row);

/// Takes a released press on a row's control: an area's header opens or
/// closes it; a hack's header flips the hack off or on anywhere on its
/// switch and opens or closes it elsewhere; a press anywhere on a
/// parameter's switch flips it. A locked row changes nothing but its
/// header's opening.
///
/// @param[in,out] dialog the dialog
/// @param row the row
/// @param x the pointer's column, in source pixels from the dialog's left edge
/// @return what the release asks of the host
DialogAction release_on(Dialog& dialog, const geometry::ListRow& row, int32_t x);

/// Takes Space on a focused row: an area or a hack opens or closes; a
/// parameter's switch flips.
///
/// @param[in,out] dialog the dialog
/// @param row the row
/// @return what the key asks of the host
DialogAction activate(Dialog& dialog, const geometry::ListRow& row);

/// Takes Left or Right on a focused row: an area closes or opens, a hack or
/// a parameter's switch turns off or on, a slider moves one stop.
///
/// @param[in,out] dialog the dialog
/// @param row the row
/// @param up true for Right
/// @return what the key asks of the host
DialogAction step(Dialog& dialog, const geometry::ListRow& row, bool up);

/// Sets a slider to the stop under a column.
///
/// @param[in,out] dialog the dialog
/// @param row the slider's row
/// @param column the column
/// @return what it asks of the host
DialogAction drag_to(Dialog& dialog, const geometry::ListRow& row, int32_t column);

/// Sets Show Active Only's switch On or Off.
///
/// @param[in,out] dialog the dialog
/// @param on true for On
/// @return DialogAction::redraw
DialogAction set_active_only(Dialog& dialog, bool on) noexcept;

/// Takes Restore profile values: clears every override, so that the
/// profile's own values show and play; only while Developer Mode is on.
///
/// @param[in,out] dialog the dialog
/// @return DialogAction::changed when there were overrides, else DialogAction::redraw
DialogAction restore_profile_values(Dialog& dialog) noexcept;

/// Tells whether Restore profile values takes a press: while Developer Mode
/// is on.
///
/// @param dialog the dialog
/// @return true when it does
[[nodiscard]] bool restore_profile_enabled(const Dialog& dialog) noexcept;

} // namespace oa::ui::engine_settings::developer
