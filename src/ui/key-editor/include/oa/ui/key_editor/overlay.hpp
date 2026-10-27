// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The keys overlay: the chords of the contexts that are active now, grouped
// by category, in a translucent panel beside the side column. It lists only
// the resolved bindings it is given, and draws only inside its panel.
#pragma once

#include "oa/ui/key_bindings/dispatcher.hpp"
#include "oa/ui/key_bindings/layout.hpp"
#include "oa/ui/key_editor/text.hpp"
#include "oa/ui/kit/canvas.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/looks.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace oa::ui::key_editor {

/// The overlay screen's automation name.
inline constexpr std::string_view name_keys_overlay = "keys-overlay";
/// The button that opens the key editor.
inline constexpr std::string_view name_keys_overlay_edit = "keys-overlay.edit";
/// The close mark.
inline constexpr std::string_view name_keys_overlay_close = "keys-overlay.close";
/// The list of groups. Its text is the title, the layout line, the On line,
/// each group's lines and the footer, one to a line.
inline constexpr std::string_view name_keys_overlay_list = "keys-overlay.list";
/// The list's scroll bar, while the list is taller than its view.
inline constexpr std::string_view name_keys_overlay_bar = "keys-overlay.bar";

/// The edit button's control number.
inline constexpr kit::ControlId id_overlay_edit = 1;
/// The close mark's control number.
inline constexpr kit::ControlId id_overlay_close = 2;
/// The list's control number.
inline constexpr kit::ControlId id_overlay_list = 3;
/// The scroll bar's control number.
inline constexpr kit::ControlId id_overlay_bar = 4;

/// One action on the overlay, or one numbered family collapsed into a range.
struct OverlayLine {
    std::string name{};  ///< the action, or the family without its number
    std::string chord{}; ///< the chord as text, or a range such as Ctrl+1…9
    /// The keycaps the chord chip draws, modifiers first. A range's last
    /// keycap holds both ends, such as 1…9.
    std::vector<std::string> keys{};
    std::string gesture{}; ///< the gesture word after the keycaps, or empty for a press
    std::string aside{};   ///< "held" for a hold or a modifier, or empty
};

/// One category of lines.
struct OverlayGroup {
    kit::KeyTint tint{kit::KeyTint::none}; ///< the category's tint
    std::string letter{};                  ///< the category letter, looked up
    std::string title{};                   ///< the category, looked up
    std::vector<OverlayLine> lines{};      ///< the actions, in the registry's order
};

/// A latch or sticky modifier that is on, as its chip shows it.
struct OverlayChip {
    std::string text{}; ///< the chip's words, looked up
    bool filled{};      ///< latched or locked; an armed modifier is outlined
};

/// What the overlay lists, and how its words are looked up.
struct OverlayInputs {
    oa::ui::key_bindings::Resolved resolved{}; ///< the bindings in use
    oa::ui::key_bindings::Contexts contexts{}; ///< the contexts and conditions active now
    std::vector<OverlayChip> on{};             ///< the latches and sticky modifiers that are on
    std::string layout_name{};                 ///< the layout's name; empty is OA default
    int32_t changes{};                         ///< how many of the player's own changes it carries
    oa::ui::key_bindings::KeyboardMap keyboard{}; ///< what the keys print
    oa::ui::key_bindings::Platform platform{};    ///< which key Primary means
    TextHooks text{};                             ///< how a word is looked up
};

/// Where the panel may go: the battlefield, and the side column's right edge.
struct OverlayRoom {
    kit::Rect battlefield{}; ///< the battlefield, in points
    int32_t column_right{};  ///< the side column's right edge, in the same points
    kit::SizeClass size_class{kit::SizeClass::compact}; ///< the class the panel is laid out at
};

/// The panel and the display list that draws it.
struct OverlayLayout {
    kit::Rect panel{};       ///< where the panel sits, in the room's points
    kit::DisplayList list{}; ///< what the panel draws, in the panel's own points
    int32_t radius{};        ///< the panel's corner radius, in points
    int32_t scroll{};        ///< how far the groups are scrolled, in points
    int32_t scroll_max{};    ///< the furthest they scroll, in points
};

/// Returns the overlay's title for the contexts that are active.
///
/// Placing a building wins over playing. Watching is the title while watch
/// is active and play is not. Anything else is playing.
///
/// @param inputs the contexts and the lookup
/// @return the title, looked up
[[nodiscard]] std::string overlay_title(const OverlayInputs& inputs);

/// Lists the keys of the contexts that are active now, by category.
///
/// An action is listed when one of its contexts is active, every condition
/// it names holds, and it has a key chord; its first key chord is shown.
/// Pointer and wheel actions are left out, and so is a mouse, wheel or pad
/// chord. A numbered family whose keys run in order, with the same
/// modifiers, collapses into one range; a broken run lists each member. An
/// action that exists only because a hack is on is listed under "This mod",
/// just before Game.
///
/// @param inputs the bindings, the contexts and the lookup
/// @return the groups that have a line, in category order
[[nodiscard]] std::vector<OverlayGroup> overlay_groups(const OverlayInputs& inputs);

/// Lays the overlay out beside the side column.
///
/// The panel sits a little right of the column and below the battlefield's
/// top: 240 points wide with one column of groups at Compact, 420 at
/// Regular and 520 at Large with two, narrower when the battlefield is.
/// It is as tall as its content, up to the battlefield less its margins;
/// beyond that the groups scroll under a fixed header and footer. `scroll`
/// is held to what the groups can scroll.
///
/// @param inputs the words and the lookup
/// @param groups the groups overlay_groups returned
/// @param room the battlefield, the column and the class
/// @param fonts the fonts the text is measured in
/// @param scroll how far the groups are scrolled, in points
/// @return the panel, its display list, its radius and the scroll
[[nodiscard]] OverlayLayout overlay_layout(
    const OverlayInputs& inputs,
    const std::vector<OverlayGroup>& groups,
    const OverlayRoom& room,
    const kit::Fonts& fonts,
    int32_t scroll
);

/// Draws the overlay from the panel's top left corner: the HUD's panel
/// colour blended over what lies under it, with rounded corners and a
/// hairline edge, then the display list. Nothing is drawn outside the panel.
///
/// @param canvas where it is drawn; its placement is the panel's corner
/// @param laid the layout overlay_layout returned
void draw_overlay(const kit::Canvas& canvas, const OverlayLayout& laid);

} // namespace oa::ui::key_editor
