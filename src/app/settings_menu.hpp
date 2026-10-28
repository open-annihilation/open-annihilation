// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Settings screen on the OA layer
// (runtime_settings_menu.cpp opens it from the game's own main menu), drawn
// with the game's own art (game_menu_art.hpp). It covers the whole window
// with the bare picture the game's menus stand on: a title in the game's
// font, unsmoothed, where the Options screen has its OPTIONS, a CATEGORIES
// panel, the chosen category's panel and a DETAILS panel, then the Options
// screen's Restore Default, Undo Changes, Cancel and OK, each on its plate,
// as the game draws them up and held.
//
// The categories list the game's own options, Sound, Music, Graphics and
// Game Speeds, and Open Annihilation's sections. A category's panel holds up
// to two sections, each under a strip: CLASSIC OPTIONS, the options the
// game's Options screen sets, marked 3.1c, and OPEN ANNIHILATION, the
// engine's own. A row shows its label and its control: a value between the
// game's two arrows, a slider with its value, buttons, or a value alone. The
// row under the pointer is lit amber, and DETAILS says what it does, its
// default and when it takes effect.
//
// The three panels stand in the frame the game draws round its score board,
// its corners and sides as the game draws them, on its dark green ground,
// each labelled in the game's font. Within them the categories and a row's
// buttons are the game's plain buttons, a section's title and a selector's
// value stand in its sunk bar, the arrows, sliders and scroll bar are its
// own, and the captions, labels and values are in its font.
//
// The screen is modal and draws nothing that moves on its own. What the
// presses do is its host's: the screen asks for a step, a stop, a button,
// a category, and the four buttons, and reads what the settings show again.
#pragma once

#include "game_menu_art.hpp"
#include "oa_layer.hpp"

#include "oa/app/window_icon.hpp"

#include <cstddef>
#include <cstdint>
#include <array>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app {

/// The Settings screen's name on the OA layer: the settings dialog's, so that
/// the screens that look for Settings find either.
inline constexpr std::string_view settings_menu_screen_name = "settings";

/// What a row's control is.
enum class SettingsMenuControl : uint8_t {
    selector, ///< its value between two arrows that step it: a switch, a strip, a drop-down
    slider,   ///< a track with a knob at its stop and its value beside it
    buttons,  ///< its value, when it has one, and up to three buttons
    text,     ///< its value alone; nothing to press
};

/// One row of a section.
struct SettingsMenuRow {
    std::string id{};                              ///< its word, for automation
    std::string label{};                           ///< what it sets, as shown
    SettingsMenuControl control{};                 ///< its control
    std::string value{};                           ///< what it shows now, as shown
    int32_t index{};                               ///< a selector's or a slider's stop now, from 0
    int32_t count{};                               ///< how many stops it has
    std::vector<std::string> buttons{};            ///< a row of buttons' captions, left to right
    bool enabled{true};                            ///< it takes a press
    std::string lock{};                            ///< why it does not, as shown; empty when it does
    std::string detail{};                          ///< what it does, as shown
    std::string default_value{};                   ///< its default, as shown; empty for none
    std::string takes_effect{};                    ///< when a change takes effect, as shown

    friend bool operator==(const SettingsMenuRow&, const SettingsMenuRow&) = default;
};

/// One section of a category's panel, under its strip.
struct SettingsMenuSection {
    std::string title{};                ///< the strip's title, as shown
    std::string badge{};                ///< the small plate at the strip's right; empty for none
    std::vector<SettingsMenuRow> rows{}; ///< its rows, top to bottom

    friend bool operator==(const SettingsMenuSection&, const SettingsMenuSection&) = default;
};

/// The picture beside a category's name.
enum class SettingsMenuIcon : uint8_t {
    sound,
    music,
    graphics,
    speeds,
    controls,
    interface,
    language,
    limits,
    touch,
    controller,
    game_files,
    downloads,
    content,
    system,
    developer,
    page, ///< a page an extension registered
};

/// One entry of the CATEGORIES panel.
struct SettingsMenuCategory {
    std::string id{};        ///< its word, for automation
    std::string name{};      ///< its name, as shown
    SettingsMenuIcon icon{}; ///< its picture
    bool after_divider{};    ///< it stands under the divider, with the files and the system

    friend bool operator==(const SettingsMenuCategory&, const SettingsMenuCategory&) = default;
};

/// What the Settings screen shows now.
struct SettingsMenuFacts {
    std::vector<SettingsMenuCategory> categories{}; ///< the entries, top to bottom
    std::size_t chosen{};                            ///< the category shown
    std::string heading{};                           ///< the chosen category's panel's label
    std::vector<SettingsMenuSection> sections{};     ///< its sections
    std::string notice{};  ///< a line in amber under the details, such as a folder that did not open

    friend bool operator==(const SettingsMenuFacts&, const SettingsMenuFacts&) = default;
};

/// What the Settings screen's presses do. The screen reads the facts again
/// after each.
struct SettingsMenuActions {
    /// Shows a category, by its place in the list.
    std::function<void(std::size_t)> choose{};
    /// Steps a row one stop, by its section and its place there: true for
    /// the next stop, false for the one before.
    std::function<void(std::size_t, std::size_t, bool)> step{};
    /// Moves a slider's row to a stop, by its section and its place there.
    std::function<void(std::size_t, std::size_t, int32_t)> set_stop{};
    /// Presses a row's button, by the row's section, its place there and
    /// the button's place from 0 at the left.
    std::function<void(std::size_t, std::size_t, std::size_t)> press{};
    std::function<void()> restore_defaults{}; ///< RESTORE DEFAULTS
    std::function<void()> undo{};             ///< UNDO: back to what the screen opened with
    std::function<void()> cancel{};           ///< CANCEL: back to that, and the screen closes
    std::function<void()> accept{};           ///< OK: the settings kept, and the screen closes
};

/// The game's own art the Settings screen is drawn with: the game's menu
/// art, and its Options screen's (STARTOPT.GUI) buttons.
struct SettingsMenuGameArt {
    GameMenuArt common{}; ///< the picture, the title, the frame and the gadgets' pieces
    /// RESTORE DEFAULTS, UNDO, CANCEL and OK, left to right: the Options
    /// screen's Restore Default, Undo Changes, Cancel and OK, each on its
    /// plate, clear around it, first up, then held.
    std::array<std::array<WindowIcon, 2>, 4> buttons{};
};

/// Makes the Settings screen.
///
/// @param shows tells whether the screen shows; it closes when it does not
/// @param facts reads what the settings show now
/// @param actions what its presses do
/// @param game_art the game's art it is drawn with
/// @return the screen
[[nodiscard]] std::unique_ptr<LayerScreen> make_settings_menu(
    std::function<bool()> shows,
    std::function<SettingsMenuFacts()> facts,
    SettingsMenuActions actions,
    SettingsMenuGameArt game_art
);

} // namespace oa::app
