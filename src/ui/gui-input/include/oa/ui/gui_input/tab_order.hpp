// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// One Tab order and one key rule for the built-in screens and for a
// GadgetPanel. The arrows still move by place (focus_nearest). Tab follows
// this order.

#include "oa/ui/gui_layout.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace oa::ui::gui_input {

/// What a key does to the focused gadget, shared by the frontend screens and a GadgetPanel.
enum class FocusKeyEffect : int32_t {
    unhandled = 0,       ///< the caller keeps what the key does today
    tab_next,            ///< Tab, along tab_order
    tab_previous,        ///< Shift+Tab, along tab_order
    scroll_back,         ///< Left, one knob position toward the bar's start
    scroll_forward,      ///< Right, one knob position toward the bar's end
    scroll_page_back,    ///< Page Up, scroll_page_span positions back
    scroll_page_forward, ///< Page Down, scroll_page_span positions forward
    scroll_home,         ///< Home, the bar's start
    scroll_end,          ///< End, the bar's end
    list_previous,       ///< Up, the list's previous row
    list_next,           ///< Down, the list's next row
    colour_forward,      ///< Right on a colour, the press that cycles forward
    colour_back,         ///< Left on a colour, the press that cycles back
};

/// The focused gadget and the key, for focus_key_action.
struct FocusKeyQuery {
    const ui::gui_layout::Gadget* gadget = nullptr; ///< the focused gadget; null when none is
    int32_t key = 0;                                ///< a key_code value
    bool shift = false;                             ///< Shift is held; Tab then goes backward
    bool ring_shown = false;                        ///< the family's focus ring is showing
};

/// Decides what a key does to the focused gadget.
///
/// Tab always moves along the order. A horizontal scroll bar steps, pages
/// and jumps to its ends. A list steps with Up and Down. A colour cycles
/// with Left and Right while the ring shows. A text box's caret keys, and
/// every other key, are left to the caller.
///
/// @param query the focused gadget and the key
/// @return the effect, or unhandled when the caller keeps today's behaviour
[[nodiscard]] FocusKeyEffect focus_key_action(const FocusKeyQuery& query) noexcept;

/// The Tab order of a screen's gadgets, as indices into `gadgets`.
///
/// A gadget is a stop when it can take the keyboard focus and has area.
/// Where the screen is one this order knows (the single-player columns, a
/// new campaign, load game, skirmish, map selection, the options screens,
/// a briefing, the campaign's end), the stops follow that screen. Every
/// other screen is read in columns: gadgets whose left edges are within 16
/// of a column share it, columns run left to right, and each column runs
/// top to bottom. A horizontal run that the columns split, whose centres
/// sit within 8 of the run's first centre and whose next control starts
/// within 40 of the one before it, is read left to right as one row.
/// When a file already lists its stops in that order, the result is the
/// file's order.
///
/// @param gadgets the screen's gadgets, the root at index 0
/// @return the stop indices, root excluded; empty when nothing can take focus
[[nodiscard]] std::vector<int32_t> tab_order(std::span<const ui::gui_layout::Gadget> gadgets);

} // namespace oa::ui::gui_input
