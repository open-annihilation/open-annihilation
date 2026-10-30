// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Updates' rows (updates_page.cpp): the update centre's card, or the status
// row a host without the card fills, and the policy rows: Check for updates,
// Download automatically, Install updates and Previous version. The dialog's
// events (dialog.cpp) call these for that section's rows; the rows are built
// when the page is laid out. The lines the status row shows are update_lines,
// declared on the dialog so a host can fill them.
#pragma once

#include "oa/ui/engine_settings/dialog.hpp"
#include "oa/ui/kit/rows.hpp"

#include "geometry.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace oa::ui::engine_settings::updates {

/// What CHECK NOW asks the host for. Checked before the folder buttons' actions.
inline constexpr int32_t check_updates_action = 10;
/// What UPDATE… asks the host for: the release's page, or the download.
inline constexpr int32_t open_release_action = 11;
/// What RESTART TO UPDATE asks the host for.
inline constexpr int32_t restart_update_action = 12;
/// What RETRY asks the host for.
inline constexpr int32_t retry_update_action = 13;
/// What Roll back… asks the host for.
inline constexpr int32_t roll_back_action = 14;
/// What the card's first button asks the host for; its buttons ask this and
/// the numbers after it, by their place from the left. Clear of every other
/// row's action.
inline constexpr int32_t first_card_action = 30;
/// The card's button that is the check CHECK NOW asks for, by its word.
inline constexpr std::string_view check_now_id = "check-now";

/// Places Updates' rows from the dialog's offer, from the class's first row.
///
/// The card, or the status row, and the check are always placed. Download
/// automatically and Install updates are placed where the host shows a
/// download (UpdatesOffer::show_download). Previous version is placed only
/// while a version is kept, and that row locks during a game. The card
/// stands over the first row, which takes its height.
///
/// @param dialog the dialog
/// @param locks what cannot be changed now; Previous version locks in a game
/// @param width how wide a text is in the small font; empty estimates
/// @param[out] placed the kit's rows, when not null
/// @param sizes the dialog's sizes; null uses the class's own
/// @param regular how wide a text is in the regular font; null leaves a
///     drop-down at the class's width and measures the card's words in the small font
/// @return the rows: the card or the status, Download automatically, the
///     check, Install updates, then Previous version while one is kept
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

/// Tells whether an Updates row is a drop-down: the check, or Install updates.
///
/// @param row the row
/// @return true for those two
[[nodiscard]] bool is_choice(const geometry::Row& row) noexcept;

/// Returns how many ways a drop-down offers.
///
/// @param row the drop-down's row
/// @return 3 for the check, 2 for Install updates
[[nodiscard]] int32_t choice_count(const geometry::Row& row) noexcept;

/// Returns the way a drop-down shows, as an index among its ways.
///
/// @param dialog the dialog
/// @param row the drop-down's row
/// @return the index, from 0
[[nodiscard]] int32_t choice_index(const Dialog& dialog, const geometry::Row& row) noexcept;

/// Returns one way's caption, in English.
///
/// @param row the drop-down's row
/// @param index the way, from 0
/// @return its caption; empty past the last
[[nodiscard]] std::string choice_caption(const geometry::Row& row, int32_t index);

/// Returns one way's id.
///
/// @param row the drop-down's row
/// @param index the way, from 0
/// @return automatically, when-asked or never; ask or when-i-quit; empty past the last
[[nodiscard]] std::string choice_id(const geometry::Row& row, int32_t index);

/// Returns the word a drop-down's items are named by to automation.
///
/// @param row the drop-down's row
/// @return update-check or update-install
[[nodiscard]] std::string_view choice_word(const geometry::Row& row) noexcept;

/// Sets a drop-down to one of its ways. The check's button follows the way
/// at once; the stored preferences wait for OK.
///
/// @param[in,out] dialog the dialog
/// @param row the drop-down's row
/// @param index the way, from 0; past the last changes nothing
void apply_choice(Dialog& dialog, const geometry::Row& row, int32_t index);

/// Shows the button the way allows, after the way changed or was put back.
///
/// @param[in,out] dialog the dialog
void follow_mode(Dialog& dialog) noexcept;

/// Takes one of the card's buttons: Check now is the check CHECK NOW asks
/// for; every other action is the host's, its number left in
/// Dialog::update_act.
///
/// @param[in,out] dialog the dialog
/// @param index the button, from the left, from 0
/// @return DialogAction::check_updates, update_action, or none past the last
[[nodiscard]] DialogAction take_card(Dialog& dialog, std::size_t index);

/// Takes a row's action when it is one of the card's buttons'.
///
/// @param[in,out] dialog the dialog
/// @param action what a row's button asked for
/// @return what take_card asks; nothing for an action that is not the card's
[[nodiscard]] std::optional<DialogAction> card_action(Dialog& dialog, oa::ui::kit::ActionId action);

/// Returns which of the card's buttons a control is, when it is the second
/// or the third; the first is the card's row control.
///
/// @param control the control
/// @return 1 or 2; nothing for any other control
[[nodiscard]] std::optional<std::size_t> card_button(int32_t control) noexcept;

/// Returns the control of one of the card's buttons.
///
/// @param index the button, from the left, from 0
/// @param row_control the card's row control, the first button's
/// @return the control
[[nodiscard]] int32_t card_control(std::size_t index, int32_t row_control) noexcept;

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
