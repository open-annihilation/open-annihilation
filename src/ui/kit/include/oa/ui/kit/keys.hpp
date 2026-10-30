// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Provisional: this header changes whenever OA's screens need it to, until the kit is declared stable (src/ui/kit/README.md).

// Keycaps, chord chips, keyboard and mouse pictures, and the hold ring.
// Labels, short names and category letters are the caller's: nothing here
// looks a word up. The keyboard's geometry is keyed by W3C code strings.
#pragma once

#include "oa/ui/kit/canvas.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/looks.hpp"
#include "oa/ui/kit/text.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace oa::ui::kit {

/// A keycap's height, in points, at every size class.
inline constexpr int32_t keycap_height = 16;

/// A keycap's least width, in points, at every size class. A small badge is
/// narrower: keycap_width returns at least 9 for one.
inline constexpr int32_t keycap_least_width = 16;

/// Returns a category tint's face.
///
/// @param tint the category; none is the untinted key
/// @return the face colour
[[nodiscard]] Colour tint_face(KeyTint tint) noexcept;

/// Returns a category tint's darker bottom edge.
///
/// @param tint the category; none is the untinted key
/// @return the edge colour
[[nodiscard]] Colour tint_edge(KeyTint tint) noexcept;

/// Returns the ink a category tint's letter and label are drawn in.
///
/// on_accent on a tinted face, and the text colour on none.
///
/// @param tint the category; none is the untinted key
/// @return the ink
[[nodiscard]] Colour tint_ink(KeyTint tint) noexcept;

/// Returns a keycap's width: its label, and at least keycap_least_width, or
/// 9 for a small badge.
///
/// @param fonts the fonts
/// @param look the keycap
/// @return the width, in points
[[nodiscard]] int32_t keycap_width(const Fonts& fonts, const KeycapLook& look);

/// Draws a keycap. A tinted face uses the tint, with a darker bottom edge.
/// The label sits at the top left and the short name along the bottom, cut
/// with the kit's ellipsis. The letter sits in a corner square and a mark
/// in the top right. A clash dashes the border in the danger colour and a
/// fixed key dashes it in the control border. A held key fills with the
/// accent. Focus draws the focus ring inside the rectangle. A small badge
/// is the accent letter on on_accent. Nothing is drawn outside the rectangle.
///
/// fill_small_mark fills the key with its mark's colour when the corner
/// mark would be smaller than that mark's glyph. The keyboard picture asks
/// for this below 4 points a quarter. Other callers leave it false.
///
/// @param canvas where it is drawn
/// @param rect the keycap, in points
/// @param look what the keycap shows
/// @param fill_small_mark fill a mark the corner glyph cannot show
void draw_keycap(
    const Canvas& canvas, const Rect& rect, const KeycapLook& look, bool fill_small_mark = false
);

/// Appends a keycap: one item and, for a control id, one control of kind
/// button that takes no steps, its text the label, enabled unless the
/// keycap is disabled. no_control adds the item alone. A control's text
/// names a Mac modifier in words: ⌘ Cmd reads Cmd.
///
/// @param[in,out] list the display list
/// @param rect the keycap, in points
/// @param look what the keycap shows
/// @param id the control's number, or no_control for the item alone
/// @param name the control's automation name
void add_keycap(
    DisplayList& list, const Rect& rect, const KeycapLook& look, ControlId id, std::string name
);

/// Returns a chord chip's width: its keycaps, the "+" between them and the
/// gesture, or the add form, or the unbound text.
///
/// @param fonts the fonts
/// @param look the chip
/// @return the width, in points
[[nodiscard]] int32_t chord_chip_width(const Fonts& fonts, const ChordChipLook& look);

/// Draws a chord chip. Keycaps are joined by "+" with the gesture word
/// after them. The add form is a dashed "+ Add key" with its caption in
/// none_text. none_text alone is the quiet unbound text. A mark is drawn
/// on the last keycap. Nothing is drawn outside the rectangle.
///
/// @param canvas where it is drawn
/// @param rect the chip, in points
/// @param look what the chip shows
void draw_chord_chip(const Canvas& canvas, const Rect& rect, const ChordChipLook& look);

/// Appends a chord chip: one item and, for a control id, one control of
/// kind button that takes no steps, its text the keycaps joined by "+" and
/// the gesture, each Mac modifier in words (⌘ Cmd+A reads Cmd+A).
/// no_control adds the item alone.
///
/// @param[in,out] list the display list
/// @param rect the chip, in points
/// @param look what the chip shows
/// @param id the control's number, or no_control for the item alone
/// @param name the control's automation name
void add_chord_chip(
    DisplayList& list, const Rect& rect, const ChordChipLook& look, ControlId id, std::string name
);

/// One key of a keyboard picture, in quarter units: four quarters are one
/// unit. The picture is 91 quarters wide and 25 high.
struct KeyPlace {
    std::string_view code{};  ///< the W3C code
    std::string_view label{}; ///< the printing the geometry draws when no cap is given
    int16_t x{};              ///< the key's left, in quarter units
    int16_t y{};              ///< the key's top, in quarter units
    int16_t w{};              ///< the key's width, in quarter units
    int16_t h{};              ///< the key's height, in quarter units
};

/// Returns the keys of a keyboard shape, in reading order. ANSI, ISO (with
/// IntlBackslash and a tall Enter) and JIS (with IntlYen, IntlRo,
/// NonConvert, Convert, KanaMode and a shorter space). Each has the
/// function row, the navigation block, the arrows and the numpad.
///
/// @param shape which keyboard
/// @return the places; valid for the rest of the program
[[nodiscard]] std::span<const KeyPlace> keyboard_places(KeyboardShape shape);

/// Returns the keyboard picture's size at one point a quarter unit.
///
/// @return 91 by 25 points
[[nodiscard]] Point keyboard_size() noexcept;

/// The main block of every keyboard shape, in quarter units: the rows from
/// Tab to Enter, Caps Lock to Enter and Shift to Shift. A picture whose look
/// asks for the main block draws the keys inside it alone.
inline constexpr Rect keyboard_main_block{0, 9, 60, 12};

/// Returns a picture's size at one point a quarter unit: the whole keyboard,
/// or its main block.
///
/// @param look the picture
/// @return 91 by 25 points, or 60 by 12 for the main block
[[nodiscard]] Point keyboard_size(const KeyboardLook& look) noexcept;

/// Tells whether a picture draws a key: every key of the whole keyboard,
/// and the keys inside keyboard_main_block for the main block.
///
/// @param place the key
/// @param look the picture
/// @return true when the picture draws it
[[nodiscard]] bool keyboard_draws(const KeyPlace& place, const KeyboardLook& look) noexcept;

/// Draws a keyboard picture scaled to fit the rectangle, whole points per
/// quarter unit; the main block alone when the look asks for it, scaled on
/// its own. A code with no entry in the look is a free key with its
/// geometry's own label. Below 4 points a quarter, neighbouring keys keep
/// a gap, or a one-point border in the free key's edge colour where a gap
/// would take the whole key. A label there is drawn whole or left out, and
/// when most of a row's labels cannot be drawn whole the picture's free
/// keys are drawn without labels. A marked key is filled with its mark's
/// colour when the corner mark would be smaller than its glyph. No legend
/// is cut: one too wide for the regular font is drawn in the small font,
/// and from 4 points a quarter a point nearer the key's sides where it
/// needs that; a Mac modifier whose legend leads with its symbol (⌘ Cmd)
/// keeps the symbol alone, at every scale; a named key whose legend still
/// does not fit takes its geometry's own label (Ent for a long Enter), then
/// a shorter one (Cap for Caps, PU for PgUp), or no legend. A command key
/// the look names (Cmd on a Mac) never takes the geometry's Win. Nothing is
/// drawn outside the rectangle.
///
/// @param canvas where it is drawn
/// @param rect the picture, in points
/// @param look which shape, and the keys that are not free
void draw_keyboard(const Canvas& canvas, const Rect& rect, const KeyboardLook& look);

/// Appends a keyboard picture: one item, and one control of kind button per
/// key it draws, in reading order. Each key's number is first plus its place
/// in keyboard_places, so a key keeps its number in the main block. A
/// control is named with the picture's name, a dot and the code word, or
/// with the code word alone when name is empty; its text is the key's label,
/// a Mac modifier in words.
///
/// @param[in,out] list the display list
/// @param rect the picture, in points
/// @param look which shape, and the keys that are not free
/// @param first the first key's control number
/// @param name the picture's automation name; each key's name hangs from it
void add_keyboard(
    DisplayList& list, const Rect& rect, const KeyboardLook& look, ControlId first, std::string name
);

/// Draws a mouse picture: a body, the buttons and the wheel. The extra
/// buttons are drawn only when the look asks for them. Nothing is drawn
/// outside the rectangle.
///
/// @param canvas where it is drawn
/// @param rect the picture, in points
/// @param look the buttons' looks, and whether the extra buttons show
void draw_mouse(const Canvas& canvas, const Rect& rect, const MouseLook& look);

/// Appends a mouse picture: one item, and one control of kind button per
/// button and one for the wheel, in the order left, right, middle, wheel,
/// back, forward, then 6, 7 and 8 when the extra buttons show. The first
/// control's number is first. Names hang from name: mouse-left, mouse-right,
/// mouse-middle, wheel, mouse-back, mouse-forward, mouse-6, mouse-7 and
/// mouse-8.
///
/// @param[in,out] list the display list
/// @param rect the picture, in points
/// @param look the buttons' looks, and whether the extra buttons show
/// @param first the first button's control number
/// @param name the picture's automation name
void add_mouse(
    DisplayList& list, const Rect& rect, const MouseLook& look, ControlId first, std::string name
);

/// Draws a hold ring filled clockwise from the top to done/whole. While
/// reduced_motion is set the fill snaps down to a quarter of the ring.
/// Nothing is drawn outside the rectangle.
///
/// @param canvas where it is drawn
/// @param rect the ring's square, in points; the shorter side is the ring
/// @param look how far the ring has filled
void draw_hold_ring(const Canvas& canvas, const Rect& rect, const HoldRingLook& look);

/// Appends a hold ring: one item and no control.
///
/// @param[in,out] list the display list
/// @param rect the ring, in points
/// @param look how far the ring has filled
void add_hold_ring(DisplayList& list, const Rect& rect, const HoldRingLook& look);

/// Returns a W3C code as the kebab words an automation name uses.
///
/// KeyA is key-a, Digit1 is digit-1, NumpadAdd is numpad-add and F12 is f12.
///
/// @param code the W3C code
/// @return the words, in lower case, hyphens between them
[[nodiscard]] std::string code_word(std::string_view code);

} // namespace oa::ui::kit
