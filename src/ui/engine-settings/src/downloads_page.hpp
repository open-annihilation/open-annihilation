// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Downloads' rows (downloads_page.cpp): the check for content updates, one
// install-ID switch and value per registry the host offers, the downloaded
// files, and, on the main menu, REGISTRIES IN THE LIBRARY…. The dialog's
// events (dialog.cpp) call these for that section's rows; the rows are
// built when the page is laid out.
#pragma once

#include "oa/ui/engine_settings/dialog.hpp"
#include "oa/ui/kit/rows.hpp"

#include "geometry.hpp"

#include <cstdint>
#include <string>

namespace oa::ui::engine_settings::downloads {

/// What RESET asks the host for. Checked before the folder buttons' actions.
inline constexpr int32_t reset_install_id_action = 4;
/// What EMPTY asks the host for.
inline constexpr int32_t empty_downloads_action = 5;
/// What REGISTRIES IN THE LIBRARY… asks for: the Library's Registries tab.
/// It follows every other row's button's action. The page lists the row only while the Library can open
/// (Dialog::library_link).
inline constexpr int32_t manage_registries_action = 7;

/// Places Downloads' rows from the dialog's offer.
///
/// @param dialog the dialog
/// @param locks what cannot be changed now; a game locks RESET and EMPTY
/// @param width how wide a text is in the small font; empty estimates
/// @param[out] placed the kit's rows, when not null
/// @param sizes the dialog's sizes; null uses the class's own
/// @param regular how wide a text is in the regular font; null leaves a
///     drop-down at the class's width
/// @return the rows, one for the check, two for each install ID, one for the
///     files, and on the main menu one for the Library's Registries tab
[[nodiscard]] geometry::Rows place(
    const Dialog& dialog,
    const Locks& locks,
    const geometry::TextWidth& width,
    oa::ui::kit::PlacedRows* placed,
    const geometry::Sizes* sizes = nullptr,
    const geometry::TextWidth* regular = nullptr
);

/// Returns what a Downloads row is.
///
/// @param dialog the dialog
/// @param row the row
/// @return its kind; text for a row that is not Downloads'
[[nodiscard]] oa::ui::kit::RowKind kind(const Dialog& dialog, const geometry::Row& row);

/// Tells whether a Downloads row is the check for content updates.
///
/// @param row the row
/// @return true for that drop-down
[[nodiscard]] bool is_choice(const geometry::Row& row) noexcept;

/// Returns how many ways the check for content updates offers.
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
/// @return automatically, library or never; empty past the last
[[nodiscard]] std::string choice_id(int32_t index);

/// Sets the check to one of its ways.
///
/// @param[in,out] dialog the dialog
/// @param index the way, from 0; past the last changes nothing
void apply_choice(Dialog& dialog, int32_t index);

/// Takes Space on a Downloads row.
///
/// @param[in,out] dialog the dialog
/// @param row the row
/// @return what the key asks; a RESET names Dialog::install_id_registry, and
///     REGISTRIES IN THE LIBRARY… asks the host to open the Library
[[nodiscard]] oa::ui::kit::RowResult activate(Dialog& dialog, const geometry::Row& row);

/// Takes a released press on a Downloads row.
///
/// @param[in,out] dialog the dialog
/// @param row the row
/// @param x the pointer's column, in source pixels from the dialog's left edge
/// @param y the pointer's row
/// @return what the release asks; a RESET names Dialog::install_id_registry,
///     and REGISTRIES IN THE LIBRARY… asks the host to open the Library
[[nodiscard]] oa::ui::kit::RowResult
release(Dialog& dialog, const geometry::Row& row, int32_t x, int32_t y);

/// Takes Left or Right on a Downloads row: the check moves one way, a
/// switch turns off or on. RESET and EMPTY take no step.
///
/// @param[in,out] dialog the dialog
/// @param row the row
/// @param up true for Right
/// @return DialogAction::changed when the check moved, else DialogAction::redraw
[[nodiscard]] DialogAction step(Dialog& dialog, const geometry::Row& row, bool up);

} // namespace oa::ui::engine_settings::downloads
