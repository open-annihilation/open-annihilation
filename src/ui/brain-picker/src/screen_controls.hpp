// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The brain picker's control numbers, which its layout uses. Private to
// the module. The names a journey presses are screen.hpp's.
#pragma once

#include "oa/ui/kit/layout.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace oa::ui::brain_picker::controls {

/// The list.
inline constexpr kit::ControlId list = 1;
/// Compact's DETAILS.
inline constexpr kit::ControlId details = 2;
/// The details page's BACK.
inline constexpr kit::ControlId back = 3;
/// CANCEL.
inline constexpr kit::ControlId cancel = 4;
/// USE THIS.
inline constexpr kit::ControlId use = 5;
/// The header's close mark.
inline constexpr kit::ControlId close_mark = 6;
/// The hint label.
inline constexpr kit::ControlId hint_label = 7;
/// The summary label.
inline constexpr kit::ControlId summary_label = 8;
/// The details' name, so a text-only pane still has a scroll clip.
inline constexpr kit::ControlId name_label = 9;
/// The list's scroll bar.
inline constexpr kit::ControlId list_bar = 10;
/// The details' scroll bar.
inline constexpr kit::ControlId details_bar = 11;
/// The level strip.
inline constexpr kit::ControlId level = 12;
/// "Use for every computer seat".
inline constexpr kit::ControlId every_seat = 13;

/// The first row. The row at place p is first_row + p.
inline constexpr kit::ControlId first_row = 1000;
/// The first option control of the selected brain.
inline constexpr kit::ControlId first_option = 2000;
/// The first item of the open menu.
inline constexpr kit::ControlId first_menu_item = 3000;

/// The list's scroll group.
inline constexpr int32_t list_group = 0;
/// The details' scroll group.
inline constexpr int32_t details_group = 1;

/// Returns a text as one word of an automation name: letters a to z and
/// digits kept, capitals made small, every other run of characters one
/// hyphen, none at either end, at most 40 bytes. "x" when nothing is kept.
///
/// @param text the text, in UTF-8
/// @return the word
[[nodiscard]] inline std::string name_word(std::string_view text) {
    constexpr std::size_t most = 40;
    std::string word;
    bool hyphen = false;
    for (const unsigned char character : text) {
        const bool small = character >= 'a' && character <= 'z';
        const bool capital = character >= 'A' && character <= 'Z';
        const bool digit = character >= '0' && character <= '9';
        if (small || digit || capital) {
            if (hyphen && !word.empty())
                word += '-';
            hyphen = false;
            word +=
                capital ? static_cast<char>(character - 'A' + 'a') : static_cast<char>(character);
        } else {
            hyphen = true;
        }
        if (word.size() >= most)
            break;
    }
    if (word.empty())
        word = "x";
    return word;
}

} // namespace oa::ui::brain_picker::controls
