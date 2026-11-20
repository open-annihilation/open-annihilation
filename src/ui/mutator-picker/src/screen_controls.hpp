// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The mutator picker's control numbers, which its layout uses. Private to
// the module. The names a journey presses are screen.hpp's.
#pragma once

#include "oa/ui/kit/layout.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace oa::ui::mutator_picker::controls {

/// Compact's filter drop-down.
inline constexpr kit::ControlId filter = 1;
/// The All pill.
inline constexpr kit::ControlId filter_all = 2;
/// The Installed pill.
inline constexpr kit::ControlId filter_installed = 3;
/// The Downloads pill.
inline constexpr kit::ControlId filter_downloads = 4;
/// TAGS.
inline constexpr kit::ControlId tags = 5;
/// The search field.
inline constexpr kit::ControlId search = 6;
/// The list.
inline constexpr kit::ControlId list = 7;
/// ▲ UP.
inline constexpr kit::ControlId up = 8;
/// ▼ DOWN.
inline constexpr kit::ControlId down = 9;
/// Compact's DETAILS.
inline constexpr kit::ControlId details = 10;
/// The details page's BACK.
inline constexpr kit::ControlId back = 11;
/// CANCEL.
inline constexpr kit::ControlId cancel = 12;
/// USE THESE.
inline constexpr kit::ControlId use = 13;
/// Read-only's CLOSE.
inline constexpr kit::ControlId done = 14;
/// The header's close mark.
inline constexpr kit::ControlId close_mark = 15;
/// The hint label.
inline constexpr kit::ControlId hint_label = 16;
/// The summary label.
inline constexpr kit::ControlId summary_label = 17;
/// The verdict label.
inline constexpr kit::ControlId verdict_label = 18;
/// The list's scroll bar.
inline constexpr kit::ControlId list_bar = 19;
/// The details' scroll bar.
inline constexpr kit::ControlId details_bar = 20;
/// The clash mode, on a mutator that brings units or weapons.
inline constexpr kit::ControlId clash = 21;
/// The filter pills, one Tab stop. Its parts are the three pills.
inline constexpr kit::ControlId filters_group = 22;
/// The tag pills, one Tab stop. Tags, the overflow menu, stays its own stop.
inline constexpr kit::ControlId tags_group = 23;

/// The first row. The row at place p is first_row + p.
inline constexpr kit::ControlId first_row = 1000;
/// The first row's drag handle.
inline constexpr kit::ControlId first_grip = 2000;
/// The first row's switch.
inline constexpr kit::ControlId first_switch = 3000;
/// The first option control of the selected mutator.
inline constexpr kit::ControlId first_option = 4000;
/// The first tag pill, for a tag that fits on the filter row.
inline constexpr kit::ControlId first_tag_pill = 5000;
/// The first item of the open menu.
inline constexpr kit::ControlId first_menu_item = 6000;

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

} // namespace oa::ui::mutator_picker::controls
