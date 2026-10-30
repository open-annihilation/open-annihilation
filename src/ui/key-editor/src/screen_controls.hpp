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
/// Same place on every keyboard, under the capture's More options.
inline constexpr kit::ControlId capture_means = 26;
/// The capture's More options.
inline constexpr kit::ControlId capture_more = 27;
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
/// The details' Settle… on a draft clash.
inline constexpr kit::ControlId card_settle = 38;
/// The clash question's confirm button.
inline constexpr kit::ControlId clash_confirm = 39;
/// The capture prompt's title. Its text is the prompt, for automation.
inline constexpr kit::ControlId capture_title = 40;
/// The line under the well.
inline constexpr kit::ControlId capture_status = 41;
/// The clash prompt's title. Its text is the prompt, for automation.
inline constexpr kit::ControlId clash_title = 42;
/// The List tab.
inline constexpr kit::ControlId view_list = 43;
/// The Keyboard tab.
inline constexpr kit::ControlId view_keyboard = 44;
/// The Mouse tab.
inline constexpr kit::ControlId view_mouse = 45;
/// The Nothing layer.
inline constexpr kit::ControlId layer_none = 46;
/// The Shift layer.
inline constexpr kit::ControlId layer_shift = 47;
/// The Ctrl layer.
inline constexpr kit::ControlId layer_ctrl = 48;
/// The Alt layer.
inline constexpr kit::ControlId layer_alt = 49;
/// The Cmd layer.
inline constexpr kit::ControlId layer_cmd = 50;
/// The keyboard shape drop-down.
inline constexpr kit::ControlId keyboard_shape = 51;
/// Right-click on the scheme strip.
inline constexpr kit::ControlId scheme_right = 52;
/// Left-click on the scheme strip.
inline constexpr kit::ControlId scheme_left = 53;
/// FIND ON KEYBOARD.
inline constexpr kit::ControlId detail_find = 54;
/// The key card. It takes no focus.
inline constexpr kit::ControlId key_card = 55;
/// IMPORT… in the footer.
inline constexpr kit::ControlId import_keys = 56;
/// SHARE… in the footer.
inline constexpr kit::ControlId share_keys = 57;
/// PASTE… in the footer.
inline constexpr kit::ControlId paste_keys = 58;
/// The share prompt's name field.
inline constexpr kit::ControlId share_name = 59;
/// COPY AS TEXT.
inline constexpr kit::ControlId share_copy = 60;
/// SAVE FILE….
inline constexpr kit::ControlId share_save = 61;
/// OPEN FOLDER.
inline constexpr kit::ControlId share_open = 62;
/// The share prompt's CLOSE.
inline constexpr kit::ControlId share_close = 63;
/// The shared text. It takes no focus.
inline constexpr kit::ControlId share_text = 64;
/// The paste prompt's rows.
inline constexpr kit::ControlId paste_list = 65;
/// APPLY {n} CHANGES.
inline constexpr kit::ControlId paste_apply = 66;
/// The paste prompt's CANCEL.
inline constexpr kit::ControlId paste_cancel = 67;
/// A mod's Ctrl+F2 keys.
inline constexpr kit::ControlId import_eye = 68;
/// tdraw's Settings.ini.
inline constexpr kit::ControlId import_tdraw = 69;
/// cnc-ddraw's ddraw.ini.
inline constexpr kit::ControlId import_ddraw = 70;
/// CHOOSE ANOTHER FILE….
inline constexpr kit::ControlId import_choose = 71;
/// The import prompt's rows.
inline constexpr kit::ControlId import_list = 72;
/// IMPORT {n} KEYS.
inline constexpr kit::ControlId import_apply = 73;
/// The import prompt's CANCEL.
inline constexpr kit::ControlId import_cancel = 74;
/// The id the share name folds to.
inline constexpr kit::ControlId share_id = 75;
/// The paste prompt's summary.
inline constexpr kit::ControlId paste_summary = 76;
/// The import prompt's summary.
inline constexpr kit::ControlId import_summary = 77;
/// The scroll bar of a share, paste or import text block.
inline constexpr kit::ControlId exchange_bar = 78;
/// Find by key, beside the search.
inline constexpr kit::ControlId find = 79;
/// Find by key's well.
inline constexpr kit::ControlId find_well = 80;
/// Find by key's Close.
inline constexpr kit::ControlId find_close = 81;
/// Find by key's line. Its text is the line, for automation.
inline constexpr kit::ControlId find_title = 82;
/// Undo, in the footer.
inline constexpr kit::ControlId undo = 83;
/// The footer's draft line.
inline constexpr kit::ControlId draft = 84;
/// The clash question's Choose another key.
inline constexpr kit::ControlId clash_another = 85;
/// The details' More options.
inline constexpr kit::ControlId detail_more = 86;
/// Show as list, on the keyboard view.
inline constexpr kit::ControlId show_as_list = 87;
/// The keyboard view's list of every key.
inline constexpr kit::ControlId key_list = 88;
/// The inspector's Change {action}'s key….
inline constexpr kit::ControlId card_change = 89;
/// The inspector's Use {key} for {action}….
inline constexpr kit::ControlId card_use = 90;
/// The key list's scroll bar.
inline constexpr kit::ControlId key_list_bar = 91;
/// The close question's words. Its text is the question, for automation.
inline constexpr kit::ControlId leave_title = 92;
/// The close question's Save.
inline constexpr kit::ControlId leave_save = 93;
/// The close question's Discard.
inline constexpr kit::ControlId leave_discard = 94;
/// The close question's Keep editing.
inline constexpr kit::ControlId leave_keep = 95;
/// The Clone and the Rename and Describe questions' name field.
inline constexpr kit::ControlId layout_name_field = 96;
/// Their description field.
inline constexpr kit::ControlId layout_description_field = 97;
/// Their main button.
inline constexpr kit::ControlId layout_ok = 98;
/// Their Cancel.
inline constexpr kit::ControlId layout_cancel = 99;
/// Their title, what the new layout starts from, and why a name cannot be used:
/// a label, shown only while their question is, so it sits after the nav's
/// entries, which never show with it.
inline constexpr kit::ControlId layout_status = 399;

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
/// Find by key's rows start at find_row_first, so a menu item stays below that.
inline constexpr kit::ControlId menu_first = 3000;
/// One row of Find by key's list. The row's place, from 0, is added.
inline constexpr kit::ControlId find_row_first = 5800;
/// One key of the keyboard picture. The key's place in keyboard_places, from 0, is added.
inline constexpr kit::ControlId keyboard_first = 6000;
/// One button of the mouse picture, in the order add_mouse numbers them.
inline constexpr kit::ControlId mouse_first = 6300;
/// One row of the mouse table, in table order.
inline constexpr kit::ControlId mouse_table_first = 6400;

} // namespace oa::ui::key_editor::controls
