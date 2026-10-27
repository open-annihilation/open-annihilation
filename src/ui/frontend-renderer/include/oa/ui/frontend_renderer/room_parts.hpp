// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// Open Annihilation's own parts in a 3.1c room (the skirmish setup, the
// battle room, the new-campaign screen), drawn from the room's own art as
// the room draws its buttons, lamps and lists: a button on the room's button
// plate in its caption font, a notch at a button's right end, a lamp, a list
// box, a line of label text and a small engraved OA stamp for a part 3.1c
// does not have. Everything is drawn in the room frame's pixels on a
// RoomCanvas, which keeps the room's own pixels where no part is drawn and
// marks the pixels a part covers. A mod's art and palette carry through.

#include "oa/formats/gaf.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/ui/frontend_renderer.hpp"
#include "oa/ui/frontend_renderer/artless.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::ui::frontend_renderer {

/// The art a room's parts are drawn with.
struct RoomArt {
    /// The room's loaded art: its caption and label fonts, its light and
    /// shade tables, its own sprites and the shared COMMONGUI.GAF. Null
    /// draws the parts that need no art: the plain button face, the notch,
    /// the stamp and the frame of a list.
    const ScreenResources* resources{};
    PaletteBytes palette{}; ///< the palette the room is shown in
};

/// Returns the palette a screen is shown in: its background's own palette,
/// else its GUI palette, as the screen's draw takes it.
///
/// @param resources the screen's resources
/// @return the palette
[[nodiscard]] PaletteBytes room_palette(const ScreenResources& resources) noexcept;

/// A picture of a room's parts, in the room frame's pixels.
struct RoomCanvas {
    SourceRect area{};              ///< the frame pixels the canvas covers
    Surface surface{};              ///< its pixels, area.width by area.height
    std::vector<uint8_t> covered{}; ///< a byte a pixel: nonzero where a part is drawn
};

/// Makes a canvas over part of a room's frame. Its pixels start as the
/// frame's, black where the frame does not reach, and no pixel is covered.
///
/// @param area the frame pixels the canvas covers; an empty one makes an empty canvas
/// @param frame the room's frame as last drawn; null starts every pixel black
/// @return the canvas
[[nodiscard]] RoomCanvas room_canvas(SourceRect area, const Surface* frame);

/// Writes a canvas as RGBA pixels, top row first: a covered pixel opaque,
/// the rest clear.
///
/// @param canvas the canvas
/// @param[out] rgba the pixels, resized to area.width x area.height x 4
void room_canvas_rgba(const RoomCanvas& canvas, std::vector<uint8_t>& rgba);

/// Turns UTF-8 words into the bytes the room's fonts draw: the game's code
/// page, or UTF-8 where game text holds it (present::encode_game_text).
///
/// @param words the words, in UTF-8
/// @return the bytes
[[nodiscard]] std::string room_text(std::string_view words);

/// Which of the room's fonts a line of text is drawn in.
enum class RoomFont : uint8_t {
    caption, ///< the buttons' caption font (hattfont12), ScreenResources::font
    label,   ///< the labels' font (hattfont11), ScreenResources::label_font
};

/// Returns the rows a line of a room font takes: its 'I' and two more.
///
/// @param art the art
/// @param font which font
/// @return the rows; 0 without the font
[[nodiscard]] int32_t room_line_height(const RoomArt& art, RoomFont font) noexcept;

/// Measures room text as draw_room_text draws it.
///
/// @param art the art
/// @param font which font
/// @param text the text, in the room's bytes (room_text)
/// @return the width, in pixels; 0 without the font
[[nodiscard]] int32_t room_text_width(const RoomArt& art, RoomFont font, std::string_view text);

/// Cuts room text to a width as a room cuts a caption too wide for its
/// button, ending what is left in "..." when anything was cut.
///
/// @param art the art
/// @param font which font
/// @param text the text, in the room's bytes
/// @param width the widest it may be, in pixels
/// @return the text, whole when it fits
[[nodiscard]] std::string
fit_room_text(const RoomArt& art, RoomFont font, std::string_view text, int32_t width);

/// Draws a line of room text as a room draws a label: the glyphs alone, in
/// their own colours, on what is under them. The pixels the text changes
/// are covered.
///
/// @param[in,out] canvas the canvas
/// @param art the art
/// @param font which font
/// @param text the text, in the room's bytes
/// @param x the pen column, in frame pixels
/// @param y the text's top row, in frame pixels
/// @param width the widest it may draw, in pixels; glyphs past it are left out
/// @param light_level a light-table row the glyphs are lit through; 0 draws them as they are
/// @return the pen column after the text
int32_t draw_room_text(
    RoomCanvas& canvas,
    const RoomArt& art,
    RoomFont font,
    std::string_view text,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t light_level = 0
);

/// Copies part of a picture into the canvas and covers it: the room's
/// background under a cell, laid over what the room drew in the cell.
///
/// @param[in,out] canvas the canvas
/// @param rect the frame pixels
/// @param picture the picture, such as the room's background; pixels it lacks are left
/// @param picture_x the frame column of the picture's left edge
/// @param picture_y the frame row of the picture's top edge
void cover_room_area(
    RoomCanvas& canvas,
    SourceRect rect,
    const Image& picture,
    int32_t picture_x = 0,
    int32_t picture_y = 0
);

/// A room's button: its plate, its caption, and the room kept at its ends
/// for a stamp, a notch or a lamp.
struct RoomButtonLook {
    SourceRect rect{};          ///< the button, in frame pixels
    std::string_view caption{}; ///< the caption, in the room's bytes (room_text)
    ButtonCondition condition{ButtonCondition::normal}; ///< pressed draws it sunk
    /// The button's own art, drawn at its own size at the button's corner,
    /// as the room draws a button that has its own sequence; null takes
    /// COMMONGUI.GAF's BUTTONS0, the art of a button with none.
    const formats::gaf::Sequence* art{};
    int32_t inset_left{};  ///< pixels kept clear inside the left end
    int32_t inset_right{}; ///< pixels kept clear inside the right end
    /// The caption may be drawn in the label font, which is narrower, when
    /// the caption font does not fit; either is then cut with "...".
    bool narrower_font{};
};

/// Returns a caption as a button draws it in a box of a width: whole in the
/// caption font where it fits the box less a caption's margin, else, where
/// the label font is allowed, whole in that, else cut with "..." in the last
/// font tried.
///
/// @param art the art
/// @param caption the caption, in the room's bytes
/// @param box_width the box's width, in pixels
/// @param narrower_font the label font may be tried
/// @return the bytes drawn
[[nodiscard]] std::string room_caption_shown(
    const RoomArt& art, std::string_view caption, int32_t box_width, bool narrower_font
);

/// Returns the width of the box an on and off button centres its caption
/// in, as draw_room_switch lays it out.
///
/// @param art the art
/// @param look the button
/// @return the width, in pixels
[[nodiscard]] int32_t room_switch_caption_width(const RoomArt& art, const RoomButtonLook& look);

/// The size group of BUTTONS0 a button of a size takes, as a button with
/// no art of its own takes it: the first frame of each group of four whose
/// width and height differ least from the button's.
///
/// @param sequence the BUTTONS0 sequence
/// @param width the button's width
/// @param height the button's height
/// @return the group's first frame; 0 for an empty sequence
[[nodiscard]] std::size_t
room_button_group(const formats::gaf::Sequence& sequence, int32_t width, int32_t height) noexcept;

/// Draws a room's button: the plate, at the button's size, then the caption
/// centred in the room left between the insets, one pixel down and right
/// while pressed. A plate from BUTTONS0 whose size is not the button's keeps
/// its ends and corners and repeats its middle. Without art the button is
/// the game's plain face: filled, with its raised or sunken bevel.
///
/// @param[in,out] canvas the canvas
/// @param art the art
/// @param look the button
void draw_room_button(RoomCanvas& canvas, const RoomArt& art, const RoomButtonLook& look);

/// The lamp a room's on and off button lights.
enum class RoomLamp : uint8_t {
    unlit, ///< neither lamp: the art's third frame
    green, ///< the green lamp, which the room lights for On: the art's second frame
    red,   ///< the red lamp, which the room lights for Off: the art's first frame
};

/// The parts of a room's on and off button, in frame pixels.
struct RoomLampParts {
    SourceRect lamp{};     ///< the lamp, at the plate's right end; empty without the art
    int32_t inset_right{}; ///< pixels from the plate's right edge to the lamp's left
};

/// Returns where a room's on and off button has its lamps: the pixels of
/// COMMONGUI.GAF's stagebuttn1 that its red and green frames do not share,
/// at the plate's right end.
///
/// @param art the art
/// @param rect the button, in frame pixels
/// @return the parts; an empty lamp without the art
[[nodiscard]] RoomLampParts room_lamp_parts(const RoomArt& art, SourceRect rect);

/// Draws a room's on and off button, with its lamps, as the room draws a
/// rule that is on or off (stagebuttn1): the plate with the lamp asked for
/// lit, and the caption centred between the insets and the lamps; grayed,
/// the whole button is darkened as a grayed button is. A plate whose size
/// is not the button's keeps its ends and repeats its middle. Without that
/// art the button is drawn by draw_room_button, and the lamp beside its
/// right end by draw_room_lamp.
///
/// @param[in,out] canvas the canvas
/// @param art the art
/// @param look the button; its art is not read
/// @param lamp the lamp
void draw_room_switch(
    RoomCanvas& canvas, const RoomArt& art, const RoomButtonLook& look, RoomLamp lamp
);

/// Returns the width an on and off button needs for its caption in the
/// caption font, with the room kept at its left and its lamp's end at its
/// right, as draw_room_switch lays them out.
///
/// @param art the art
/// @param caption the caption, in the room's bytes
/// @param inset_left pixels kept clear inside the left end
/// @return the width, in pixels
[[nodiscard]] int32_t
room_switch_width(const RoomArt& art, std::string_view caption, int32_t inset_left);

/// Draws a lamp alone, in the room's palette: a dark well, and lit from
/// light at its top to darker at its foot.
///
/// @param[in,out] canvas the canvas
/// @param art the art
/// @param rect the lamp, in frame pixels
/// @param lamp the lamp
void draw_room_lamp(RoomCanvas& canvas, const RoomArt& art, SourceRect rect, RoomLamp lamp);

/// Draws a bar that fills as work goes on: a dark well sunk into the room,
/// filled from its left with the room's green lamp light as far as the work
/// has come.
///
/// @param[in,out] canvas the canvas
/// @param art the art
/// @param rect the bar, in frame pixels
/// @param done how far the work has come, from 0 to whole
/// @param whole the whole; 0 or less fills nothing
void draw_room_bar(
    RoomCanvas& canvas, const RoomArt& art, SourceRect rect, int32_t done, int32_t whole
);

/// Returns the notch at a button's right end: as wide as asked, inside the
/// plate's two-pixel edge.
///
/// @param button the button, in frame pixels
/// @param width the notch's width
/// @return the notch, in frame pixels
[[nodiscard]] SourceRect room_notch_rect(SourceRect button, int32_t width) noexcept;

/// Draws the notch at a button's right end: a groove down its left side and
/// a triangle, in the caption font's colour, pointing down, or up while
/// what it opens is open. Held, the triangle drops a pixel and the groove
/// turns.
///
/// @param[in,out] canvas the canvas
/// @param art the art
/// @param notch the notch, room_notch_rect
/// @param open what the notch opens is open
/// @param held a press is held on the notch
void draw_room_notch(
    RoomCanvas& canvas, const RoomArt& art, SourceRect notch, bool open, bool held
);

/// Draws the small engraved OA stamp that marks a part 3.1c does not have:
/// a dark sunk plate with "OA" cut in the palette's gold, the thin letters
/// below 14 pixels high and the bold ones from there.
///
/// @param[in,out] canvas the canvas
/// @param art the art
/// @param rect the stamp, in frame pixels
void draw_room_stamp(RoomCanvas& canvas, const RoomArt& art, SourceRect rect);

/// Returns the stamp's place inside a button's left end: square to the
/// button's height less its edges, at most 12 pixels high, as wide as the
/// letters need.
///
/// @param button the button, in frame pixels
/// @return the stamp, in frame pixels
[[nodiscard]] SourceRect room_stamp_rect(SourceRect button) noexcept;

/// Lights the focus marker round a record, as the room lights it round its
/// own: six outlines a pixel apart through light levels 31 down to 6, over
/// what the canvas holds. The pixels lit are covered.
///
/// @param[in,out] canvas the canvas
/// @param art the art; without a light table nothing is lit
/// @param record the record, in frame pixels
void draw_room_focus(RoomCanvas& canvas, const RoomArt& art, SourceRect record);

/// One row of a room's list box.
struct RoomListRow {
    std::string text{};  ///< the row's words, in the room's bytes, at its left
    std::string value{}; ///< words at its right, in the room's bytes; empty for none
    bool heading{};      ///< a heading in the label font, which cannot be chosen
    bool
        selected{}; ///< the row the pointer or the keys are on, lit as the room lights a list's selection
    bool ticked{};     ///< a tick before the words: what the seat plays now
    bool grey{};       ///< a row that cannot be chosen, darkened as a grayed button is
    bool checkbox{};   ///< the value column shows a check box
    bool checked{};    ///< the check box is ticked
    bool rule_above{}; ///< a groove is drawn above the row
};

/// How a room's list box is drawn.
struct RoomListLook {
    SourceRect rect{};                   ///< the box, its frame included, in frame pixels
    std::span<const RoomListRow> rows{}; ///< the rows, top to bottom
    int32_t value_width{};               ///< the value column's width, in pixels
};

/// Pixels between a list box's edge and its rows, on every side.
inline constexpr int32_t room_list_border = 5;
/// Pixels a groove above a row takes.
inline constexpr int32_t room_list_rule = 5;
/// Pixels the words of every row but a heading start from the row's left
/// edge, leaving room for a tick.
inline constexpr int32_t room_list_indent = 12;

/// Returns the rows a list row takes: the caption font's line height and
/// one, as a list whose items have no height of their own takes.
///
/// @param art the art
/// @return the rows; at least 12
[[nodiscard]] int32_t room_list_row_height(const RoomArt& art) noexcept;

/// Returns where each row of a list box lies.
///
/// @param art the art
/// @param look the box
/// @return one rectangle a row, in frame pixels, the value column included
[[nodiscard]] std::vector<SourceRect>
room_list_row_rects(const RoomArt& art, const RoomListLook& look);

/// Returns the size a list box needs for its rows: the widest row's words
/// and the value column beside them, and every row's height, with the
/// frame round them.
///
/// @param art the art
/// @param rows the rows
/// @param value_width the value column's width, in pixels
/// @return the width and height, as a rectangle at 0, 0
[[nodiscard]] SourceRect
room_list_size(const RoomArt& art, std::span<const RoomListRow> rows, int32_t value_width);

/// Draws a room's list box: a frame on the room's button face round a dark
/// well, and in it the rows in the caption font, a heading in the label
/// font, a tick before a ticked row's words, the selected row lit through
/// light level 30 as the room lights a list's selection, a grey row
/// darkened, a groove above a row that asks for one, and a value column at
/// the right: its words right-aligned, or a check box from COMMONGUI.GAF's
/// CHECKBOX art, its first frame off and its second on.
///
/// @param[in,out] canvas the canvas
/// @param art the art
/// @param look the box
void draw_room_list(RoomCanvas& canvas, const RoomArt& art, const RoomListLook& look);

} // namespace oa::ui::frontend_renderer
