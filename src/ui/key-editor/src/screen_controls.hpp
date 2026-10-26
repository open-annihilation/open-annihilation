// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The key editor's control numbers. The layout gives them and the input
// reads them. They are not an automation name: a journey presses the names
// in screen.hpp.
#pragma once

#include "oa/ui/kit/layout.hpp"

namespace oa::ui::key_editor::controls {

inline constexpr kit::ControlId layout_choice = 1;
inline constexpr kit::ControlId search = 2;
inline constexpr kit::ControlId category = 3;
inline constexpr kit::ControlId filter_all = 4;
inline constexpr kit::ControlId filter_changed = 5;
inline constexpr kit::ControlId filter_clashes = 6;
inline constexpr kit::ControlId filter_unbound = 7;
inline constexpr kit::ControlId filter_latchable = 8;
inline constexpr kit::ControlId list = 9;
inline constexpr kit::ControlId number_keys = 10;
inline constexpr kit::ControlId detail_latch = 11;
inline constexpr kit::ControlId detail_confirm = 12;
inline constexpr kit::ControlId detail_reset = 13;
inline constexpr kit::ControlId back = 14;
inline constexpr kit::ControlId more = 15;
inline constexpr kit::ControlId restore = 16;
inline constexpr kit::ControlId cancel = 17;
inline constexpr kit::ControlId save = 18;
inline constexpr kit::ControlId close_mark = 19;
inline constexpr kit::ControlId summary = 20;
inline constexpr kit::ControlId footer_note = 21;
inline constexpr kit::ControlId list_bar = 22;
inline constexpr kit::ControlId details_bar = 23;

/// One nav entry. The category's place among the nav's entries, from 0, is added.
inline constexpr kit::ControlId nav_first = 100;
/// One list row. The row's place among the rows the list could show, from 0, is added.
inline constexpr kit::ControlId row_first = 400;
/// A details chord's remove control. The chord's place, from 0, is added.
/// The automation name's number is that place plus 1.
inline constexpr kit::ControlId chord_first = 2000;
/// One item of the open menu. The item's place, from 0, is added.
inline constexpr kit::ControlId menu_first = 3000;

} // namespace oa::ui::key_editor::controls
