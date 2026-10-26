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
/// + ADD KEY, under the chosen action's chords.
inline constexpr kit::ControlId detail_add = 24;
/// The capture prompt's well. A button or a wheel turn here is the chord.
inline constexpr kit::ControlId capture_well = 25;
/// Means: the letter.
inline constexpr kit::ControlId capture_letter = 26;
/// Means: the key's place.
inline constexpr kit::ControlId capture_place = 27;
/// Starts on: a press.
inline constexpr kit::ControlId capture_press = 28;
/// Starts on: a double press.
inline constexpr kit::ControlId capture_double = 29;
/// Starts on: a hold.
inline constexpr kit::ControlId capture_hold = 30;
/// The capture prompt's CANCEL.
inline constexpr kit::ControlId capture_cancel = 31;
/// The capture prompt's OK.
inline constexpr kit::ControlId capture_ok = 32;
/// The clash prompt's REPLACE.
inline constexpr kit::ControlId clash_replace = 33;
/// The clash prompt's SWAP.
inline constexpr kit::ControlId clash_swap = 34;
/// The clash prompt's CANCEL.
inline constexpr kit::ControlId clash_cancel = 35;
/// The draft card's KEEP IT HERE.
inline constexpr kit::ControlId card_here = 36;
/// The draft card's KEEP IT FOR.
inline constexpr kit::ControlId card_there = 37;
/// The draft card's CHOOSE ANOTHER KEY.
inline constexpr kit::ControlId card_choose = 38;
/// The chord chip in the search, with its clear mark.
inline constexpr kit::ControlId search_chord = 39;
/// The capture prompt's title. Its text is the prompt, for automation.
inline constexpr kit::ControlId capture_title = 40;
/// The line under the well.
inline constexpr kit::ControlId capture_status = 41;
/// The clash prompt's title. Its text is the prompt, for automation.
inline constexpr kit::ControlId clash_title = 42;

/// One nav entry. The category's place among the nav's entries, from 0, is added.
inline constexpr kit::ControlId nav_first = 100;
/// One list row. The row's place among the rows the list could show, from 0, is added.
inline constexpr kit::ControlId row_first = 400;
/// A details chord's remove control. The chord's place, from 0, is added.
/// The automation name's number is that place plus 1.
inline constexpr kit::ControlId chord_first = 2000;
/// A details chord the player can change. Enter opens the capture. The
/// chord's place, from 0, is added. This stays below the menu.
inline constexpr kit::ControlId chord_row_first = 2500;
/// One item of the open menu. The item's place, from 0, is added.
inline constexpr kit::ControlId menu_first = 3000;

} // namespace oa::ui::key_editor::controls
