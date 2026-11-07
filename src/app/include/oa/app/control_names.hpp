// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Names for the original game's picture buttons, colour cells and sliders
// (accessibility.md §5.7). A screen reader hears the name and the value,
// "Starting metal, 1000", rather than the number alone.
#pragma once

#include "oa/app/automation_host.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {

/// How a named control's value is spoken.
enum class ControlValue : uint8_t {
    none,     ///< the name is enough
    number,   ///< the setting the screen shows, such as "1.0" or "1000"
    colour,   ///< the colour's name and the player, such as "Blue, player 1"
    side,     ///< the side the picture shows, such as "Arm"
    caption,  ///< a cycling button's current caption, such as "Game ends"
    alliance, ///< the alliance the cell shows, or none
    doors,    ///< the battle room's start doors, open or closed
};

/// How a table row names gadgets beyond one exact record.
enum class ControlKey : uint8_t {
    exact, ///< this file's gadget of this name
    star,  ///< this file's screen adds "<stem>0", "<stem>1", … at runtime
    slot,  ///< the file's "<stem>x" template and the "<stem>0" clones made from it
    added, ///< the screen creates this gadget; the GUI file does not store it
};

/// One row of the names table.
struct ControlName {
    std::string_view gui;    ///< the GUI file, such as "MAINMENU.GUI"
    std::string_view gadget; ///< the gadget, a star family, or a slot template
    std::string_view label;  ///< the words a screen reader uses for it
    ControlValue value = ControlValue::none;
    ControlKey key = ControlKey::exact;
};

/// What the screen is showing for one control, besides the table.
///
/// Strings are owned so a caller can build them and pass the reading on
/// without leaving a view into a temporary.
struct ControlReading {
    std::string gui;             ///< the GUI file this control was loaded from
    std::string linked_label;    ///< the text label whose link names this control
    std::string drawn;           ///< the number or mode a neighbouring label draws
    std::string side_name;       ///< the side a side picture shows; empty when none
    std::string shortcut;        ///< a key that works, already in the player's layout
    int quick_key = 0;           ///< the GUI file's quick-key byte; 0 when it has none
    bool publish_letter = false; ///< the quick key is a letter no other control on the screen uses
    int stages = 0;              ///< a cycling button's caption count
    int colour = -1;             ///< a colour cell's frame; negative when it has none
    int player = 0;              ///< the player, from 1; 0 when the control is not a player's
    int32_t knob = 0;            ///< a slider's knob position
    int32_t range = 0;           ///< a slider's position count
    bool has_slider = false;     ///< `knob` and `range` are the bar's
    /// Neighbouring controls, the name and the text each draws. The namer
    /// reads a slider's setting from the neighbour the table names.
    std::vector<std::pair<std::string, std::string>> neighbours;
    int alliance_frame = -1;   ///< an alliance cell's frame; negative when this is not one
    bool doors_known = false;  ///< the collector knows whether the start doors are closed
    bool doors_closed = false; ///< the start doors still take the Start button's clicks
};

/// Returns how many rows the names table holds.
[[nodiscard]] std::size_t control_name_count();

/// Returns one row of the names table.
///
/// @param index the row, from 0
/// @return that row; an empty row past the end
[[nodiscard]] ControlName control_name_at(std::size_t index);

/// Finds the row for a gadget.
///
/// A HUD button kept under one side's order file matches the same button on
/// that side's other HUD files. A star row matches the numbered widgets a
/// screen adds, and a slot row matches the template and its clones. A row
/// does not name a gadget of some other screen.
///
/// @param gui the GUI file, with or without its folder
/// @param gadget the gadget's name
/// @return the row; null when the table has none
[[nodiscard]] const ControlName* find_control_name(std::string_view gui, std::string_view gadget);

/// Returns the name of the label that draws a slider's setting, when one does.
///
/// @param gadget the slider
/// @return that label's name; empty when the slider's own reading is enough
[[nodiscard]] std::string_view slider_value_label(std::string_view gadget);

/// Returns the words for a colour cell.
///
/// @param colour the colour's frame, in `kPlayerDotColors` order
/// @param player the player, from 1
/// @return "Blue, player 1"; empty when the colour or the player is not one of the ten
[[nodiscard]] std::string colour_text(int colour, int player);

/// Returns the gamma a saved setting shows, to one decimal place.
///
/// The visuals screen stores gamma as a whole number and shows it as that
/// number divided by 24, plus one half.
///
/// @param setting the stored setting, from 0
/// @return the shown gamma, such as "1.0"
[[nodiscard]] std::string gamma_text(int32_t setting);

/// Returns a slider's setting as the number the screen shows.
///
/// @param gadget the slider's name
/// @param knob its knob position
/// @param range how many positions the knob has
/// @return the setting; empty when this slider has no known scale
[[nodiscard]] std::string slider_setting_text(std::string_view gadget, int32_t knob, int32_t range);

/// Returns a quick key as a screen reader speaks it.
///
/// Only a letter is returned, in upper case. A digit, a mark or a zero is
/// not a key a player can press for that control.
///
/// @param quick_key the GUI file's quick-key byte
/// @return the letter; empty when the byte is not a letter
[[nodiscard]] std::string shortcut_letter(int quick_key);

/// Fills a control's label, value and shortcut from the names table.
///
/// A caption that already names the control is left as its label. A cycling
/// button takes the text label linked to it, and its current caption becomes
/// the value. The table's own words are the player's language. Nothing here
/// changes the control's own name or its drawn text.
///
/// @param[in,out] control the control
/// @param reading what the screen is showing for it
void apply_control_words(AutomationControl& control, const ControlReading& reading);

/// The function the multiplayer screens' collector calls, once the game has
/// installed it. Tests that collect those screens without the names table
/// leave it empty, and the collector then keeps the labels it already had.
using ControlNamer = void (*)(AutomationControl&, const ControlReading&);

/// The namer installed for the multiplayer collector. One for the program.
///
/// @return the slot the installer writes and the collector reads
[[nodiscard]] inline ControlNamer& control_namer_slot() noexcept {
    static ControlNamer installed = nullptr;
    return installed;
}

/// Installs the namer the multiplayer collector calls.
///
/// @param namer the namer; null to call none
inline void set_control_namer(ControlNamer namer) noexcept {
    control_namer_slot() = namer;
}

/// Returns the namer installed for the multiplayer collector.
[[nodiscard]] inline ControlNamer control_namer() noexcept {
    return control_namer_slot();
}

} // namespace oa::app
