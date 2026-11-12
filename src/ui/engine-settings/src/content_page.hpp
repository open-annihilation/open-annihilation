// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Game Content's rows (content_page.cpp): the sentence about the copy, one
// text row for each bundle the host listed, one row with Remove… for each
// stray archive it named, then Check again, Copy again from… and Open
// folder. The dialog's events call take for those buttons.
#pragma once

#include "oa/ui/engine_settings/dialog.hpp"
#include "oa/ui/kit/rows.hpp"

#include "geometry.hpp"

namespace oa::ui::engine_settings::content {

/// Places Game Content's rows from the bundles the host listed.
///
/// An empty list is the sentence and the three buttons. The host's rows are
/// read as they are: a bundle's name, its size and its check are already the
/// words to show. The sentence breaks between words where it is measured, as
/// every row's hint does: a no-break space joins the words beside it into
/// one, measured whole.
///
/// @param dialog the dialog
/// @param locks unused; nothing on this page is locked
/// @param width a text's width in the small font the sentence is drawn in;
///        the estimated measure counts characters
/// @param[out] placed the kit's rows, when not null
/// @param sizes the sizes the rows are placed in, as a language's wide
///        section names fit them; null for the class's own
/// @return the rows
[[nodiscard]] geometry::Rows place(
    const Dialog& dialog,
    const Locks& locks,
    const geometry::TextWidth& width,
    oa::ui::kit::PlacedRows* placed,
    const geometry::Sizes* sizes = nullptr
);

/// Returns what one of the buttons asks for.
///
/// A stray's Remove… sets Dialog::stray_pressed to that stray.
///
/// @param dialog the dialog, whose bundle and stray counts place the buttons
/// @param row the row
/// @return the button's action, or DialogAction::none for the sentence and
///     a bundle's row
[[nodiscard]] DialogAction take(Dialog& dialog, const geometry::Row& row) noexcept;

} // namespace oa::ui::engine_settings::content
