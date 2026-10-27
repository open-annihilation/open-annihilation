// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The keys overlay: the chords of the contexts that are active now, grouped
// by category, laid out beside the side column. It reads the resolved
// bindings it is given and draws nothing of its own outside that list.
#pragma once

#include "oa/ui/key_bindings/dispatcher.hpp"
#include "oa/ui/key_bindings/layout.hpp"
#include "oa/ui/key_editor/text.hpp"
#include "oa/ui/kit/canvas.hpp"
#include "oa/ui/kit/layout.hpp"

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
/// The scrolling list of groups. Its text is the title, the lines and the On line.
inline constexpr std::string_view name_keys_overlay_list = "keys-overlay.list";

/// The edit button's control number.
inline constexpr kit::ControlId id_overlay_edit = 1;
/// The close mark's control number.
inline constexpr kit::ControlId id_overlay_close = 2;
/// The list's control number.
inline constexpr kit::ControlId id_overlay_list = 3;
/// The scroll bar's control number.
inline constexpr kit::ControlId id_overlay_bar = 4;

/// One action on the overlay, or one collapsed family.
struct OverlayLine {
    std::string name{};  ///< the action, or the family with its number stripped
    std::string chord{}; ///< the chord that fires it, or a range such as Ctrl+1…9
    std::string aside{}; ///< "held", or empty
};

/// One category of lines.
struct OverlayGroup {
    kit::KeyTint tint{kit::KeyTint::none}; ///< the category
    std::string letter{};                  ///< the category letter, already looked up
    std::string title{};                   ///< the category, already looked up
    std::vector<OverlayLine> lines{};      ///< the actions, in registry order
};

/// What the overlay lists and where its words come from.
struct OverlayInputs {
    oa::ui::key_bindings::Resolved resolved{};    ///< the bindings in use
    oa::ui::key_bindings::Contexts contexts{};    ///< the contexts and conditions active now
    std::vector<std::string> on{};                ///< latches and sticky modifiers that are on
    std::string layout_name{};                    ///< the layout's name; empty is OA default
    int32_t changes{};                            ///< how many of the player's own changes
    oa::ui::key_bindings::KeyboardMap keyboard{}; ///< what the keys print
    oa::ui::key_bindings::Platform platform{};    ///< which key Primary means
    TextHooks text{};                             ///< how a word is looked up
};

/// The battlefield the panel sits in, and the side column's right edge.
struct OverlayRoom {
    kit::Rect battlefield{}; ///< the battlefield, in points
    int32_t column_right{};  ///< the side column's right edge, in the battlefield's points
    kit::SizeClass size_class{kit::SizeClass::compact}; ///< the match dialog's class
};

/// The panel and the display list that draws it. Coordinates in the list are
/// panel-local points. `panel` is in the room's points.
struct OverlayLayout {
    kit::Rect panel{};       ///< where the panel sits
    kit::DisplayList list{}; ///< what the panel draws
    int32_t scroll{};        ///< how far the list is scrolled, in points
    int32_t scroll_max{};    ///< the greatest scroll, in points
};

/// Returns the overlay's title for the contexts that are active.
///
/// Placing wins over playing. Watching is the title when watch is active and
/// play is not. Anything else is playing.
///
/// @param inputs the contexts and the lookup
/// @return the looked-up title
[[nodiscard]] std::string overlay_title(const OverlayInputs& inputs);

/// Lists the keys of the contexts that are active now, by category.
///
/// An action is listed when one of its contexts is active, its conditions all
/// hold, and its first chord is a key. Pointer and wheel actions are left
/// out, and so is a mouse, wheel or pad chord. A numbered family whose chords
/// run in order collapses into one range. An action whose sources are all
/// hack ids is grouped under "This mod", immediately before Game.
///
/// @param inputs the bindings, the contexts and the lookup
/// @return the groups that have a line, in category order
[[nodiscard]] std::vector<OverlayGroup> overlay_groups(const OverlayInputs& inputs);

/// Lays the overlay out beside the side column.
///
/// One column of 240 points at Compact, two of 420 at Regular and 520 at
/// Large, clamped so the panel stays inside the battlefield. The header and
/// the footer stay put. The groups scroll. `scroll` is clamped to what the
/// content can scroll.
///
/// @param inputs the words and the lookup
/// @param groups the groups overlay_groups returned
/// @param room the battlefield and the column
/// @param fonts the fonts the text is measured in
/// @param scroll how far the list is scrolled, in points
/// @return the panel, the display list and the scroll
[[nodiscard]] OverlayLayout overlay_layout(
    const OverlayInputs& inputs,
    const std::vector<OverlayGroup>& groups,
    const OverlayRoom& room,
    const kit::Fonts& fonts,
    int32_t scroll
);

/// Draws the overlay: the panel colour blended over the battlefield, a
/// hairline inside that rectangle, then the display list.
///
/// The canvas's placement is the panel's origin. `size` is the panel, in points.
///
/// @param canvas where it is drawn
/// @param list the display list from overlay_layout
/// @param size the panel's width and height, in points
void draw_overlay(const kit::Canvas& canvas, const kit::DisplayList& list, kit::Point size);

} // namespace oa::ui::key_editor
