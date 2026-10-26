// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Updates' rows (updates_page.cpp): this build's version and whether a
// release is out, and when Open Annihilation looks for one. The dialog's
// events (dialog.cpp) call these for that section's rows; the rows are
// built when the page is laid out. The lines the status row shows are
// update_lines, declared on the dialog so a host can fill them.
#pragma once

#include "oa/ui/engine_settings/dialog.hpp"
#include "oa/ui/kit/rows.hpp"

#include "geometry.hpp"

#include <cstdint>
#include <string>

namespace oa::ui::engine_settings::updates {

/// What CHECK NOW asks the host for. Checked before the folder buttons' actions.
inline constexpr int32_t check_updates_action = 10;
/// What UPDATE… asks the host for: the release's page, or the download.
inline constexpr int32_t open_release_action = 11;
/// What RESTART TO UPDATE asks the host for.
inline constexpr int32_t restart_update_action = 12;
/// What RETRY asks the host for.
inline constexpr int32_t retry_update_action = 13;

/// Places Updates' two rows from the dialog's offer.
///
/// @param dialog the dialog
/// @param locks what cannot be changed now; this page locks nothing
/// @param width how wide a text is in the small font; empty estimates
/// @param[out] placed the kit's rows, when not null
/// @param sizes the dialog's sizes; null uses the class's own
/// @param regular how wide a text is in the regular font; null leaves a
///     drop-down at the class's width
/// @return the rows: the status, then the check
[[nodiscard]] geometry::Rows place(
    const Dialog& dialog,
    const Locks& locks,
    const geometry::TextWidth& width,
    oa::ui::kit::PlacedRows* placed,
    const geometry::Sizes* sizes = nullptr,
    const geometry::TextWidth* regular = nullptr
);

/// Returns what an Updates row is.
///
/// @param dialog the dialog
/// @param row the row
/// @return its kind; text for a row that is not Updates'
[[nodiscard]] oa::ui::kit::RowKind kind(const Dialog& dialog, const geometry::Row& row);

/// Tells whether an Updates row is the check.
///
/// @param row the row
/// @return true for that drop-down
[[nodiscard]] bool is_choice(const geometry::Row& row) noexcept;

/// Returns how many ways the check offers.
///
/// @return 3
[[nodiscard]] int32_t choice_count() noexcept;

/// Returns the check the dialog shows, as an index among those ways.
///
/// @param dialog the dialog
/// @return 0, 1 or 2
[[nodiscard]] int32_t choice_index(const Dialog& dialog) noexcept;

/// Returns one way's caption, in English.
///
/// @param index the way, from 0
/// @return its caption; empty past the last
[[nodiscard]] std::string choice_caption(int32_t index);

/// Returns one way's id.
///
/// @param index the way, from 0
/// @return automatically, when-asked or never; empty past the last
[[nodiscard]] std::string choice_id(int32_t index);

/// Sets the check to one of its ways. The button follows the way at once;
/// the stored preference waits for OK.
///
/// @param[in,out] dialog the dialog
/// @param index the way, from 0; past the last changes nothing
void apply_choice(Dialog& dialog, int32_t index);

/// Shows the button the way allows, after the way changed or was put back.
///
/// @param[in,out] dialog the dialog
void follow_mode(Dialog& dialog) noexcept;

/// Takes Space on an Updates row.
///
/// @param[in,out] dialog the dialog
/// @param row the row
/// @return what the key asks
[[nodiscard]] oa::ui::kit::RowResult activate(Dialog& dialog, const geometry::Row& row);

/// Takes a released press on an Updates row.
///
/// @param[in,out] dialog the dialog
/// @param row the row
/// @param x the pointer's column, in source pixels from the dialog's left edge
/// @param y the pointer's row
/// @return what the release asks
[[nodiscard]] oa::ui::kit::RowResult
release(Dialog& dialog, const geometry::Row& row, int32_t x, int32_t y);

/// Takes Left or Right on an Updates row: the check moves one way. The
/// status row takes no step.
///
/// @param[in,out] dialog the dialog
/// @param row the row
/// @param up true for Right
/// @return DialogAction::changed when the check moved, else DialogAction::redraw
[[nodiscard]] DialogAction step(Dialog& dialog, const geometry::Row& row, bool up);

} // namespace oa::ui::engine_settings::updates
