// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Speech where the screen-reader bridge cannot run. A focus change is spoken
// as its label, what it is and its value. This header names no system speech
// engine, so every build can include it. The speaking itself is Windows only.
#pragma once

#include "oa/platform/screen_reader/announce.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace oa::platform::screen_reader {

/// What a line handed to the voice is.
enum class SpokenLine : uint8_t {
    focus,     ///< the control the focus moved to
    polite,    ///< an announcement that waits its turn
    assertive, ///< an announcement said at once
};

/// How long after an announcement a focus change or an assertive line waits
/// behind it instead of cutting it off, in milliseconds.
inline constexpr int64_t announcement_kept_ms = 500;

/// Whether a line stops the voice before it is spoken.
///
/// A polite announcement never does. A focus change and an assertive
/// announcement do, unless an announcement was handed to the voice less than
/// `announcement_kept_ms` ago. That line came with this one, as a message
/// box's text comes with the focus on its button, so it is heard first.
///
/// @param line what the line is
/// @param since_announcement_ms milliseconds since the last announcement was
///        handed to the voice; negative when there has been none
/// @return true when the line cuts off what the voice is saying
[[nodiscard]] bool line_cuts_off(SpokenLine line, int64_t since_announcement_ms);

/// What a focused control is, in the words a player hears.
///
/// The values match `Role` in the bridge tree, and
/// `AutomationControlRole` through `label`. `window` is the game's window.
enum class SpokenRole : uint8_t {
    none,           ///< decoration, left unspoken as a role
    button,         ///< a push button
    check_box,      ///< a box that is on or off
    switch_control, ///< a switch
    radio_group,    ///< a strip or a row, with one part chosen
    radio,          ///< one part of a strip or a row
    tab_list,       ///< a row of tabs
    tab,            ///< one tab
    list,           ///< a list of rows
    list_item,      ///< one row
    tree,           ///< a tree of rows
    tree_item,      ///< one row of a tree
    text_field,     ///< a field that takes typed text
    password,       ///< a masked field
    search,         ///< a search field
    slider,         ///< a slider, a scroll bar or a stepper
    log,            ///< a log of lines
    dialog,         ///< a dialog
    alert_dialog,   ///< a message box
    group,          ///< a group of controls
    image,          ///< a picture with a meaning
    link,           ///< a link
    label,          ///< words that take no pointer
    window,         ///< the game's window
};

/// The words for a role, as a player hears them.
///
/// Decoration has none. The words are fixed; a translation of the control's
/// own label is the label the caller passes, not this.
///
/// @param role what the control is
/// @return the words, or empty for decoration
[[nodiscard]] std::string_view role_words(SpokenRole role);

/// The sentence spoken when the focus moves to a control.
///
/// The parts that are present are the label, the role and the value, in
/// that order, separated by a comma. An empty part is left out. An empty
/// sentence is silence.
///
/// @param label the control's name; empty when it has none
/// @param role what it is, from `role_words`; empty when that is unspoken
/// @param value its value; empty when it has none
/// @return the sentence
[[nodiscard]] std::string
focus_sentence(std::string_view label, std::string_view role, std::string_view value);

/// Speaks a focus change, and stops whatever was being spoken.
///
/// The same control, with the same label and role, is spoken once. A later
/// change of only its value does not speak again. An empty sentence stops
/// the previous line and says nothing. An announcement handed to the voice
/// just before (`line_cuts_off`) is not stopped: the focus is spoken after
/// it. On Windows this speaks when
/// `speech_where_bridge_cannot` is true. Everywhere else, and when that is
/// false, it does nothing. A machine with no speech engine stays quiet and
/// shows nothing.
///
/// @param identity a stable id of the control; not spoken
/// @param label the control's name
/// @param role what it is, from `role_words`
/// @param value its value
void speak_focus(
    std::string_view identity, std::string_view label, std::string_view role, std::string_view value
);

/// Speaks one announcement.
///
/// A polite line waits for the line already being spoken. An assertive line
/// stops it, unless that is an announcement handed to the voice just before
/// (`line_cuts_off`). Does nothing when `speech_where_bridge_cannot` is
/// false, when the text is empty, and where this system does not speak. A
/// missing speech engine is silence, with no dialog and no error.
///
/// @param text the sentence
/// @param politeness how soon it is spoken
void speak_announcement(std::string_view text, Politeness politeness);

/// Forgets the focus last spoken, so the next focus is spoken again.
///
/// The preference change and a test call this. It does not speak.
void forget_spoken_focus();

} // namespace oa::platform::screen_reader
