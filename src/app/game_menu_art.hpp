// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The game's own art the Settings and Library screens are drawn with over
// the game's own main menu (runtime_game_menu_art.hpp reads it), and the
// painter that draws their parts with it: the bare picture the game's menus
// stand on, a title in the game's font where the Options screen has its
// OPTIONS, the frame the game draws round its score board and the tube
// between its parts, and the pieces of commongui.gaf the game draws its
// gadgets with: its plain buttons, a big button on its plate, its sunk bar,
// its list box, its text field, its check box, and its sliders' and scroll
// bars' pieces. Text is drawn in the game's font, unsmoothed, each of its
// pixels a square of points; buttons' captions are smoothed, as the captions
// drawn into the game's own buttons.
#pragma once

#include "menu_paint.hpp"

#include "oa/app/window_icon.hpp"
#include "oa/ui/kit/canvas.hpp"
#include "oa/ui/kit/layout.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace oa::app {

/// The pieces of the game's sliders and scroll bars, as SLIDERS holds them:
/// an upright bar's from 0, a level bar's from 10.
enum class SliderPiece : uint8_t {
    upright_start = 0,  ///< an upright bar's top
    upright_track = 1,  ///< its track, repeated down it
    upright_end = 2,    ///< its bottom
    upright_knob_start = 3, ///< its knob's top
    upright_knob_middle = 4, ///< its knob's middle, repeated down it
    upright_knob_end = 5, ///< its knob's bottom
    up = 6,             ///< its upper arrow, up; held at 7
    down = 8,           ///< its lower arrow, up; held at 9
    level_start = 10,   ///< a level bar's left end
    level_track = 11,   ///< its track, repeated along it
    level_end = 12,     ///< its right end
    level_knob = 13,    ///< its knob
    back = 16,          ///< its left arrow, up; held at 17
    next = 18,          ///< its right arrow, up; held at 19
};

/// How many pieces SLIDERS holds.
inline constexpr std::size_t slider_piece_count = 20;

/// How many pieces the game's list box (commongui.gaf's LISTBOX) is drawn
/// of: its corners, sides and middle, row by row from the top left.
inline constexpr std::size_t list_box_piece_count = 9;

/// The game's own art a screen is drawn with over the game's own main menu.
struct GameMenuArt {
    WindowIcon background{}; ///< the bare picture, which covers the screen
    WindowIcon title{};      ///< the title in the game's font, clear between its letters
    int32_t title_left{};    ///< the title's first column on the picture, in its pixels
    int32_t title_top{};     ///< the title's first row on the picture, in its pixels
    int32_t title_height{};  ///< the title's rows on the picture, in its pixels
    /// A panel's frame, as the game frames its score board (OUTCOME0): its
    /// four corners and a stretch of each side between them, joined, clear
    /// around the frame.
    WindowIcon frame{};
    /// The columns of the frame's left and right corners and the rows of its
    /// top and bottom ones, in that order: left, top, right, bottom.
    std::array<int32_t, 4> frame_corners{};
    /// How far the frame reaches past a panel's edges at each side, in the
    /// picture's pixels: the panel's edges are the frame's tube's outer ones.
    std::array<int32_t, 4> frame_reach{};
    std::array<uint8_t, 3> frame_ground{}; ///< the colour within the frame
    /// A stretch of the frame's top tube, repeated across a panel where a
    /// divider lies.
    WindowIcon rule{};
    /// How far a divider stands within a panel's left and right edges, in
    /// the picture's pixels: it runs from the frame's left tube to its right.
    std::array<int32_t, 2> rule_inset{};
    /// Draws a text in the game's font, as the title, clear between its
    /// letters: a character the font lacks as the nearest it has.
    std::function<WindowIcon(std::string_view)> caption{};
    /// The game's plain buttons (commongui.gaf's BUTTONS0), one for each
    /// size it draws them at: up, held and grayed.
    std::vector<std::array<WindowIcon, 3>> plain_buttons{};
    /// The pieces of the game's sliders and scroll bars (commongui.gaf's
    /// SLIDERS), in its order: SliderPiece names them.
    std::vector<WindowIcon> sliders{};
    /// The game's sunk bar (commongui.gaf's LIGHTBAR, its widest), the
    /// stubs at its ends cut off.
    WindowIcon well{};
    /// The game's list box (commongui.gaf's LISTBOX), black within a bevel:
    /// its pieces, row by row from the top left.
    std::array<WindowIcon, list_box_piece_count> list_box{};
    /// The game's check box (commongui.gaf's CHECKBOX): its lamp off, and on.
    std::array<WindowIcon, 2> check_box{};
    /// A plate of the Options screen a big button stands on, with no button
    /// on it, clear around it.
    WindowIcon plate{};
    /// Where the button stands on the plate, in its pixels: its first
    /// column and row, its width and its height.
    std::array<int32_t, 4> plate_slot{};
};

/// The tints the game's captions are drawn in.
enum class Ink : uint8_t {
    plain, ///< the font's own colours
    lit,   ///< amber: what lies under the pointer
    faded, ///< darkened: what cannot be changed, and a side note
};

/// How the pixels of a text in the game's font are drawn.
enum class TextEdge : uint8_t {
    sharp,    ///< unsmoothed, each a square of points
    smoothed, ///< blended between its pixels, as soft as the captions drawn into the game's own buttons
};

/// The amber of the game's lit captions, which the screens draw their own
/// amber marks in too.
inline constexpr oa::ui::kit::Colour game_amber{255, 196, 72};

/// The size of the screen the game's art screens are laid out on: their
/// panels, text and title as large as on a 1024x768 window.
inline constexpr int32_t stage_columns = 1024;
inline constexpr int32_t stage_rows = 768;

/// Returns where a screen drawn with the game's art lays out its parts on a
/// window of a size: the stage times the most whole times the window holds
/// it, once at least, cut to the window, centred.
///
/// @param width the window's width, in points
/// @param height its height, in points
/// @return the stage's place, in points
[[nodiscard]] oa::ui::kit::Rect game_menu_stage(int32_t width, int32_t height) noexcept;

/// A picture cut in three across, so that it stretches along its middle
/// alone: its left end, its middle and its right end.
struct Sliced {
    WindowIcon left{};   ///< its left end
    WindowIcon middle{}; ///< its middle, stretched between the ends
    WindowIcon right{};  ///< its right end
};

/// Draws a screen's parts with the game's art.
class GameMenuPainter {
  public:

    /// Cuts the art up for drawing.
    ///
    /// @param art the game's art
    explicit GameMenuPainter(GameMenuArt art);

    /// Tells whether every part can be drawn with the art: its buttons,
    /// bars, boxes and font are all there and cut to stretch.
    /// read_game_menu_art hands out no art without them.
    ///
    /// @return whether they are
    [[nodiscard]] bool content() const noexcept { return content_; }

    /// Returns the art.
    ///
    /// @return the art
    [[nodiscard]] const GameMenuArt& art() const noexcept { return art_; }

    /// Places the picture to cover a screen of a size, its shape kept, and
    /// the title where it stands on the picture covering the screen's stage,
    /// and sizes the screen's text to its measures; forgets what was drawn
    /// at the last size.
    ///
    /// @param width the screen's width, in points
    /// @param height its height, in points
    /// @param unit the points a row of the screen's 1080-row measures takes
    void lay_out(int32_t width, int32_t height, double unit);

    /// Returns where the picture lies, in points.
    ///
    /// @return its place
    [[nodiscard]] const oa::ui::kit::Rect& background() const noexcept { return background_; }

    /// Returns where the title stands, in points.
    ///
    /// @return its place
    [[nodiscard]] const oa::ui::kit::Rect& title() const noexcept { return title_; }

    /// Returns how many points one of the game's pictures' pixels covers: as
    /// many as on the picture covering the screen's stage.
    ///
    /// @return the points a pixel; 1 for a picture of no width
    [[nodiscard]] double picture_scale() const noexcept;

    /// Draws the picture and the title over it, unsmoothed.
    ///
    /// @param canvas where it draws
    void draw_background(const oa::ui::kit::Canvas& canvas) const;

    /// Returns a caption in the game's font, in a tint, drawn once.
    ///
    /// @param text the caption
    /// @param ink its tint
    /// @return the caption, kept as long as the painter
    [[nodiscard]] const WindowIcon& caption(std::string_view text, Ink ink) const;

    /// Returns how many points each pixel of a caption is drawn at in a box:
    /// the screen's text size, fewer where that outgrows most of the box's
    /// height or its width, one at least.
    ///
    /// @param box the box
    /// @param caption the caption
    /// @return the points a pixel
    [[nodiscard]] int32_t caption_square(const oa::ui::kit::Rect& box, const WindowIcon& caption) const noexcept;

    /// Returns how wide a text is drawn in a box.
    ///
    /// @param box the box
    /// @param text the text
    /// @return its width, in points
    [[nodiscard]] int32_t text_width(const oa::ui::kit::Rect& box, std::string_view text) const;

    /// Returns how many points each pixel of the font covers in a line of a
    /// height.
    ///
    /// @param height the line's height, in points
    /// @return the points a pixel
    [[nodiscard]] int32_t text_square(int32_t height) const;

    /// Returns the height of a box draw_text writes a line in at a size: the
    /// least whose caption_square is that size.
    ///
    /// @param square the points a pixel
    /// @return the box's height, in points
    [[nodiscard]] int32_t line_height(int32_t square) const;

    /// Returns a measure of texts drawn at a size, for wrapping them.
    ///
    /// @param square the points a pixel
    /// @return the measure, in points
    [[nodiscard]] std::function<int32_t(std::string_view)> measure(int32_t square) const;

    /// Draws a text in the game's font, unsmoothed, centred down a box, at
    /// caption_square's points a pixel; a text wider than the box at a point
    /// a pixel is cut short, an ellipsis after it.
    ///
    /// @param canvas where it draws
    /// @param box the box
    /// @param text the text
    /// @param align where the text sits along the box's width
    /// @param ink its tint
    /// @param edge how its pixels are drawn
    void draw_text(
        const oa::ui::kit::Canvas& canvas,
        const oa::ui::kit::Rect& box,
        std::string_view text,
        oa::ui::kit::Align align,
        Ink ink,
        TextEdge edge = TextEdge::sharp
    ) const;

    /// Draws a text in the game's font at a size, centred down a box: cut
    /// short, an ellipsis after it, where the box is too narrow for it.
    ///
    /// @param canvas where it draws
    /// @param box the box
    /// @param text the text
    /// @param align where the text sits along the box's width
    /// @param ink its tint
    /// @param square the points a pixel
    /// @param edge how its pixels are drawn
    void draw_text_at(
        const oa::ui::kit::Canvas& canvas,
        const oa::ui::kit::Rect& box,
        std::string_view text,
        oa::ui::kit::Align align,
        Ink ink,
        int32_t square,
        TextEdge edge = TextEdge::sharp
    ) const;

    /// Draws a picture cut in three across over a rectangle: its ends as
    /// wide as the rectangle's height makes them, its middle stretched
    /// between them.
    ///
    /// @param canvas where it draws
    /// @param rect the rectangle
    /// @param sliced the picture
    void draw_sliced(const oa::ui::kit::Canvas& canvas, const oa::ui::kit::Rect& rect, const Sliced& sliced) const;

    /// Draws one of the game's plain buttons over a rectangle: of the size
    /// whose shape is nearest the rectangle's.
    ///
    /// @param canvas where it draws
    /// @param rect the button
    /// @param frame 0 up, 1 held, 2 grayed
    void draw_plain_button(const oa::ui::kit::Canvas& canvas, const oa::ui::kit::Rect& rect, std::size_t frame) const;

    /// Draws a plain button with its caption, smoothed as the game's own
    /// buttons' captions: lit under the pointer, held while pressed, grayed
    /// when it takes no press.
    ///
    /// @param canvas where it draws
    /// @param rect the button
    /// @param caption its words
    /// @param usable it takes a press
    /// @param lit the pointer is over it
    /// @param held it is held down
    void draw_captioned_button(
        const oa::ui::kit::Canvas& canvas,
        const oa::ui::kit::Rect& rect,
        std::string_view caption,
        bool usable,
        bool lit,
        bool held
    ) const;

    /// Returns the width a big button on its plate takes at a height.
    ///
    /// @param height its height, in points
    /// @return its width, in points
    [[nodiscard]] int32_t big_button_width(int32_t height) const noexcept;

    /// Draws a big button on its plate, as the Options screen's: the plate
    /// over the rectangle, the game's plain button in its slot and the
    /// caption on it, smoothed as the game's own buttons' captions.
    ///
    /// @param canvas where it draws
    /// @param rect the plate
    /// @param caption the button's words
    /// @param usable it takes a press
    /// @param lit the pointer is over it
    /// @param held it is held down
    void draw_big_button(
        const oa::ui::kit::Canvas& canvas,
        const oa::ui::kit::Rect& rect,
        std::string_view caption,
        bool usable,
        bool lit,
        bool held
    ) const;

    /// Draws the game's sunk bar over a rectangle.
    ///
    /// @param canvas where it draws
    /// @param rect the bar
    void draw_well(const oa::ui::kit::Canvas& canvas, const oa::ui::kit::Rect& rect) const;

    /// Draws the game's list box over a rectangle: black within its bevel.
    ///
    /// @param canvas where it draws
    /// @param rect the box
    void draw_box(const oa::ui::kit::Canvas& canvas, const oa::ui::kit::Rect& rect) const;

    /// Draws the game's check box in a square: its lamp lit while on.
    ///
    /// @param canvas where it draws
    /// @param rect the square
    /// @param on it is on
    void draw_check_box(const oa::ui::kit::Canvas& canvas, const oa::ui::kit::Rect& rect, bool on) const;

    /// Draws a panel's frame: the game's, its tube along the panel's edges
    /// and its corners and bolts past them, on the game's ground.
    ///
    /// @param canvas where it draws
    /// @param panel the panel
    /// @return false when there is no frame to draw, and nothing was drawn
    bool draw_frame(const oa::ui::kit::Canvas& canvas, const oa::ui::kit::Rect& panel) const;

    /// Draws the frame's tube across a panel, from its left tube to its
    /// right, centred on a row.
    ///
    /// @param canvas where it draws
    /// @param panel the panel
    /// @param row the row the tube's middle lies on
    /// @return false when there is no tube to draw, and nothing was drawn
    bool draw_rule(const oa::ui::kit::Canvas& canvas, const oa::ui::kit::Rect& panel, int32_t row) const;

    /// Returns one of the game's slider pieces.
    ///
    /// @param piece the piece
    /// @param held the held arrow, the piece after it
    /// @return the piece
    [[nodiscard]] const WindowIcon& slider_piece(SliderPiece piece, bool held = false) const;

    /// Returns how many points a pixel of the game's slider pieces covers
    /// where a piece is to fit a room: the picture's own, fewer where the
    /// room is smaller.
    ///
    /// @param room the room, in points
    /// @param pixels the piece's pixels across it
    /// @return the points a pixel
    [[nodiscard]] double piece_scale(int32_t room, uint32_t pixels) const noexcept;

    /// Returns where one of the game's slider pieces lies, its top left at a
    /// point.
    ///
    /// @param at the point
    /// @param piece the piece
    /// @param scale the points a pixel
    /// @return its place
    [[nodiscard]] static oa::ui::kit::Rect
    piece_place(oa::ui::kit::Point at, const WindowIcon& piece, double scale) noexcept;

    /// Draws the game's scroll bar: its arrows, its track between them and
    /// its knob.
    ///
    /// @param canvas where it draws
    /// @param bar the bar, its arrows included
    /// @param up the upper arrow
    /// @param down the lower arrow
    /// @param well the track between them
    /// @param thumb the knob
    /// @param up_held the upper arrow is held
    /// @param down_held the lower arrow is held
    void draw_scroll_bar(
        const oa::ui::kit::Canvas& canvas,
        const oa::ui::kit::Rect& bar,
        const oa::ui::kit::Rect& up,
        const oa::ui::kit::Rect& down,
        const oa::ui::kit::Rect& well,
        const oa::ui::kit::Rect& thumb,
        bool up_held,
        bool down_held
    ) const;

    /// Draws a picture scaled to fill a rectangle, as the screens' own art.
    ///
    /// @param canvas where it draws
    /// @param rect the rectangle
    /// @param picture the picture; it stays as long as the painter
    void draw_picture(const oa::ui::kit::Canvas& canvas, const oa::ui::kit::Rect& rect, const WindowIcon& picture) const {
        fitted_.draw(canvas, rect, picture);
    }

  private:

    /// The pieces a panel's frame is drawn of: its four corners, then its
    /// top, bottom, left and right sides.
    enum class FramePiece : uint8_t { top_left, top_right, bottom_left, bottom_right, top, bottom, left, right };

    /// How many pieces a frame is drawn of.
    static constexpr std::size_t frame_piece_count = 8;

    /// Returns one of the frame's pieces.
    ///
    /// @param which the piece
    /// @return it
    [[nodiscard]] const WindowIcon& frame_piece(FramePiece which) const noexcept {
        return frame_pieces_[static_cast<std::size_t>(which)];
    }

    /// Repeats a piece along a rectangle, at the picture's scale.
    ///
    /// @param canvas where it draws
    /// @param along the rectangle
    /// @param piece the piece
    /// @param across along its width; down its height otherwise
    void repeat(
        const oa::ui::kit::Canvas& canvas, const oa::ui::kit::Rect& along, const WindowIcon& piece, bool across
    ) const;

    GameMenuArt art_{};                                 ///< the game's art
    int32_t text_square_{1};                            ///< the points a pixel of every text on the screen
    oa::ui::kit::Rect background_{};                    ///< where the picture lies
    int32_t staged_width_{};                            ///< the width of the picture covering the stage
    oa::ui::kit::Rect title_{};                         ///< where the title stands
    std::array<WindowIcon, frame_piece_count> frame_pieces_{}; ///< the frame, cut up
    std::vector<std::array<Sliced, 3>> plain_buttons_{}; ///< the plain buttons, cut up
    Sliced well_{};                                     ///< the sunk bar, cut up
    bool content_{};                                    ///< the content's pieces are all there
    /// The captions drawn, by their text and their tint.
    mutable std::map<std::pair<std::string, Ink>, WindowIcon> captions_{};
    menu_paint::FittedArt fitted_{};                    ///< the pieces, filtered to their sizes
    /// The smoothed captions, by the caption and the canvas pixels they
    /// cover across and down.
    mutable std::map<std::tuple<const WindowIcon*, int32_t, int32_t>, std::vector<uint8_t>> smoothed_{};
};

} // namespace oa::app
