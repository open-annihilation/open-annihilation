// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Anti-aliased painting into an RGBA canvas: rectangles, rounded panels,
// circles, the radial menu's wedges, the controls' icon marks, their text
// and the 3.1c pictures the pad's build ring shows, blended with straight
// alpha into a buffer of the layer's own, every shape's edge smoothed by how
// much of each pixel the shape covers. The touch controls, the Game files
// screen and the folder chooser draw with it, and the gamepad's glyphs are
// painted with it (docs/touch-controls.md).
#pragma once

#include "oa/platform/text_font.hpp"

#include <cstddef>
#include <span>
#include <stdint.h>
#include <string>
#include <string_view>
#include <vector>

namespace oa::ui::paint {

/// A colour with straight (not premultiplied) alpha.
struct Rgba {
    uint8_t r{}; ///< red, 0 to 255
    uint8_t g{}; ///< green, 0 to 255
    uint8_t b{}; ///< blue, 0 to 255
    uint8_t a{}; ///< opacity, 0 clear to 255 opaque

    /// Compares two colours channel by channel.
    ///
    /// @return true when every channel matches
    bool operator==(const Rgba&) const = default;
};

/// Returns a colour with its opacity multiplied by a factor.
///
/// @param colour the colour
/// @param opacity the factor, 0 to 1; values outside are clamped
/// @return the colour, its opacity scaled and rounded
[[nodiscard]] Rgba with_opacity(Rgba colour, float opacity) noexcept;

/// A rectangle of whole pixels.
struct Box {
    int x{};      ///< left column
    int y{};      ///< top row
    int width{};  ///< columns; 0 or less is empty
    int height{}; ///< rows; 0 or less is empty

    /// Returns whether the box holds no pixel.
    ///
    /// @return true when the width or the height is 0 or less
    [[nodiscard]] bool empty() const noexcept { return width <= 0 || height <= 0; }

    /// Compares two boxes field by field.
    ///
    /// @return true when every field matches
    bool operator==(const Box&) const = default;
};

/// Returns the pixels two boxes share.
///
/// @param a a box
/// @param b another box
/// @return their overlap; an empty box at 0,0 when they share none
[[nodiscard]] Box intersect(Box a, Box b) noexcept;

/// Returns the smallest box holding two boxes; an empty box adds nothing.
///
/// @param a a box
/// @param b another box
/// @return the box around both; the other when one is empty
[[nodiscard]] Box unite(Box a, Box b) noexcept;

/// A point in pixels, with fractions: pixel centres lie at half-pixel places.
struct Spot {
    float x{}; ///< columns from the left edge
    float y{}; ///< rows from the top edge
};

/// A rectangle in pixels, with fractions.
struct Area {
    float x{};      ///< left edge
    float y{};      ///< top edge
    float width{};  ///< columns
    float height{}; ///< rows
};

/// An RGBA buffer the layer is painted into, four bytes a pixel, rows top to bottom.
struct Canvas {
    int width{};               ///< pixels
    int height{};              ///< pixels
    std::vector<uint8_t> rgba; ///< width × height × 4 bytes, straight alpha
};

/// Makes a canvas of a size, every pixel clear.
///
/// @param width pixels; a negative width is taken as 0
/// @param height pixels; a negative height is taken as 0
/// @return the canvas
[[nodiscard]] Canvas make_canvas(int width, int height);

/// Returns a pixel of a canvas.
///
/// @param canvas the canvas
/// @param x column
/// @param y row
/// @return the pixel; clear for a place outside the canvas
[[nodiscard]] Rgba pixel_at(const Canvas& canvas, int x, int y) noexcept;

/// Blends a colour over a pixel with straight alpha: the colour's opacity,
/// times the share of the pixel it covers, goes over what the pixel holds.
///
/// @param[in,out] pixel four bytes: red, green, blue and opacity
/// @param colour the colour laid over it
/// @param coverage how much of the pixel the colour covers, 0 to 255
void blend_pixel(uint8_t* pixel, Rgba colour, uint32_t coverage) noexcept;

/// The marks the controls show, drawn as plain vector strokes and shapes;
/// none of them is game art.
enum class Icon : uint8_t {
    none,            ///< nothing
    shift_arrow,     ///< an outlined upward arrow: QUEUE
    plus,            ///< a plus: ADD, STORE
    cross,           ///< a diagonal cross: CLEAR, cancel, close
    box_select,      ///< a dashed square: SELECT, select of a type
    pause,           ///< two upright bars: PAUSE
    gauge,           ///< a dial with its needle: SPEED
    speech,          ///< a speech bubble: CHAT
    crosshair,       ///< a ringed crosshair: CENTRE, ATTACK
    eye,             ///< an eye: FOLLOW
    chevrons,        ///< two chevrons pointing right: NEXT
    info,            ///< a ringed letter i: INFO
    menu,            ///< three bars: MENU
    grid,            ///< four squares: BUILD
    magnifier_minus, ///< a magnifier with a minus: zoom out
    magnifier_plus,  ///< a magnifier with a plus: zoom in
    arrow,           ///< an arrow pointing right: MOVE
    patrol,          ///< two arrows pointing opposite ways: PATROL
    shield,          ///< a shield: GUARD
    square,          ///< an outlined square: STOP
    star,            ///< a five-pointed star: D-GUN
    dots,            ///< three dots in a row: MORE
    tray_down,       ///< an arrow down into a tray: RECLAIM, UNLOAD
    tray_up,         ///< an arrow up out of a tray: LOAD
    wrench,          ///< a wrench: REPAIR
    flag,            ///< a flag on its pole: CAPTURE
    warning,         ///< a triangle with an exclamation mark: SELF-DESTRUCT
    chevron_left,    ///< one chevron pointing left: back, previous page
    chevron_right,   ///< one chevron pointing right: next page
    chevron_down,    ///< one chevron pointing down: a menu that opens
    power,           ///< a power symbol: on and off
    cloak,           ///< a dashed outline of an eye: cloak
};

/// How many icons there are, none included.
inline constexpr std::size_t icon_count = static_cast<std::size_t>(Icon::cloak) + 1U;

/// Paints shapes into a canvas, inside a clipping box, and remembers the box
/// of every pixel it changed.
class Painter {
  public:

    /// Starts painting a canvas: the clip is the whole canvas and nothing is painted yet.
    ///
    /// @param canvas the canvas painted; it must outlive the painter
    explicit Painter(Canvas& canvas) noexcept;

    /// Returns the canvas painted.
    ///
    /// @return the canvas
    [[nodiscard]] Canvas& canvas() noexcept { return *canvas_; }

    /// Limits painting to a box, within the canvas.
    ///
    /// @param clip the box; the part outside the canvas is left out
    void set_clip(Box clip) noexcept;

    /// Lets painting reach the whole canvas again.
    void reset_clip() noexcept;

    /// Returns the box painting is limited to.
    ///
    /// @return the clip, inside the canvas
    [[nodiscard]] Box clip() const noexcept { return clip_; }

    /// Returns the box of every pixel painted since the painter started or
    /// forget_painted ran.
    ///
    /// @return the box; empty when nothing was painted
    [[nodiscard]] Box painted() const noexcept { return painted_; }

    /// Forgets the painted box, as if nothing had been painted.
    void forget_painted() noexcept { painted_ = {}; }

    /// Clears a box: its pixels become fully clear. The painted box does not grow.
    ///
    /// @param box the pixels cleared, inside the clip
    void clear_box(Box box) noexcept;

    /// Blends a colour over a box of whole pixels.
    ///
    /// @param box the pixels painted, inside the clip
    /// @param colour the colour
    void fill_rect(Box box, Rgba colour) noexcept;

    /// Blends the edge of a box of whole pixels, a number of pixels thick, inside the box.
    ///
    /// @param box the box
    /// @param thickness pixels; 1 or more
    /// @param colour the colour
    void outline_rect(Box box, int thickness, Rgba colour) noexcept;

    /// Blends a rectangle with rounded corners.
    ///
    /// @param area the rectangle
    /// @param radius the corners' radius in pixels; at most half the shorter side
    /// @param colour the colour
    void fill_rounded_rect(Area area, float radius, Rgba colour) noexcept;

    /// Blends the edge of a rectangle with rounded corners, inside the rectangle.
    ///
    /// @param area the rectangle
    /// @param radius the corners' radius in pixels
    /// @param thickness the edge's width in pixels
    /// @param colour the colour
    void outline_rounded_rect(Area area, float radius, float thickness, Rgba colour) noexcept;

    /// Blends a disc.
    ///
    /// @param centre the centre
    /// @param radius pixels
    /// @param colour the colour
    void fill_circle(Spot centre, float radius, Rgba colour) noexcept;

    /// Blends a ring centred on a circle.
    ///
    /// @param centre the centre
    /// @param radius the circle's radius in pixels
    /// @param thickness the ring's width in pixels
    /// @param colour the colour
    void outline_circle(Spot centre, float radius, float thickness, Rgba colour) noexcept;

    /// Blends a part of a ring between two radii and two directions: a wedge
    /// of the radial menu. Directions are clockwise from straight up.
    ///
    /// @param centre the ring's centre
    /// @param inner the inner radius in pixels; 0 makes a pie slice
    /// @param outer the outer radius in pixels
    /// @param middle the direction of the wedge's middle, radians clockwise from up
    /// @param half_sweep half the wedge's angle, radians, up to pi
    /// @param colour the colour
    void fill_sector(
        Spot centre, float inner, float outer, float middle, float half_sweep, Rgba colour
    ) noexcept;

    /// Blends the edge of a wedge (fill_sector's shape), inside it.
    ///
    /// @param centre the ring's centre
    /// @param inner the inner radius in pixels
    /// @param outer the outer radius in pixels
    /// @param middle the direction of the wedge's middle, radians clockwise from up
    /// @param half_sweep half the wedge's angle, radians
    /// @param thickness the edge's width in pixels
    /// @param colour the colour
    void outline_sector(
        Spot centre,
        float inner,
        float outer,
        float middle,
        float half_sweep,
        float thickness,
        Rgba colour
    ) noexcept;

    /// Blends a straight stroke with round ends.
    ///
    /// @param from one end
    /// @param to the other end
    /// @param width the stroke's width in pixels
    /// @param colour the colour
    void stroke_line(Spot from, Spot to, float width, Rgba colour) noexcept;

    /// Blends a stroke along part of a circle, with square-cut ends.
    ///
    /// @param centre the circle's centre
    /// @param radius the circle's radius in pixels
    /// @param middle the direction of the arc's middle, radians clockwise from up
    /// @param half_sweep half the arc's angle, radians
    /// @param width the stroke's width in pixels
    /// @param colour the colour
    void stroke_arc(
        Spot centre, float radius, float middle, float half_sweep, float width, Rgba colour
    ) noexcept;

    /// Blends a polygon, filled by the even-odd rule.
    ///
    /// @param points the corners in order; fewer than three paints nothing
    /// @param colour the colour
    void fill_polygon(std::span<const Spot> points, Rgba colour) noexcept;

    /// Blends a colour through a coverage map: each byte says how much of its pixel it covers.
    ///
    /// @param alpha width × height bytes, top row first
    /// @param width columns of the map
    /// @param height rows of the map
    /// @param x the column the map's left edge lands on
    /// @param y the row the map's top edge lands on
    /// @param colour the colour
    void draw_coverage(
        std::span<const uint8_t> alpha, int width, int height, int x, int y, Rgba colour
    ) noexcept;

    /// Blends part of an RGB picture scaled into an area: each canvas pixel whose centre lies in
    /// the area takes the picture's pixel under it (no smoothing, so pixel art stays sharp), at
    /// an opacity.
    ///
    /// @param rgb the picture, three bytes a pixel, rows top to bottom
    /// @param width the picture's width in pixels
    /// @param height the picture's height in pixels
    /// @param source the part drawn, picture pixels; the part outside the picture is left out
    /// @param area where it goes on the canvas
    /// @param opacity 0 to 1; values outside are clamped
    void draw_rgb(
        std::span<const uint8_t> rgb, int width, int height, Box source, Area area, float opacity
    ) noexcept;

    /// Blends an RGBA picture, its alpha straight, scaled to fill a box of whole
    /// pixels and filtered, so that a picture drawn at a size of its own, such as
    /// the Open Annihilation icon at one and a half times, keeps its curves.
    ///
    /// Each axis is filtered on its own. Where the picture shrinks along it, a
    /// pixel takes the picture's pixels its span covers, each weighted by how
    /// much of it the span covers; where the picture grows, the two picture
    /// pixels nearest its centre, weighted by how near each is (bilinear),
    /// the picture's edge pixels standing in past its edge. The colours are
    /// mixed weighted by their alpha, the alpha by itself, and the result is
    /// laid over the canvas by its alpha. At the picture's own size each pixel
    /// is the picture's.
    ///
    /// @param rgba the picture, four bytes a pixel, rows top to bottom
    /// @param width the picture's width in pixels
    /// @param height the picture's height in pixels
    /// @param box where it goes on the canvas, inside the clip
    void draw_rgba(std::span<const uint8_t> rgba, int width, int height, Box box) noexcept;

    /// Blends an icon centred in a box, as large as the box's shorter side allows.
    ///
    /// @param icon the icon; none paints nothing
    /// @param box the box
    /// @param colour the colour
    void draw_icon(Icon icon, Area box, Rgba colour) noexcept;

  private:

    /// Blends a shape given by its signed distance: negative inside, in pixels.
    ///
    /// @param bounds the pixels that can be inside, before clipping
    /// @param distance the shape's distance at a pixel centre
    /// @param colour the colour
    template <typename Distance>
    void fill_shape(Box bounds, Distance&& distance, Rgba colour) noexcept;

    /// Grows the painted box by a box.
    ///
    /// @param box the pixels just painted
    void note_painted(Box box) noexcept;

    Canvas* canvas_{};
    Box clip_{};
    Box painted_{};
};

/// One line of text in a style, drawn: the coverage and where it sits.
struct TextLine {
    oa::platform::text_font::Coverage coverage{}; ///< the drawn line
    bool drawn{};                                 ///< false when the stack refused the text
};

/// Draws a line of text with a font stack, antialiased, in a style.
///
/// @param fonts the font stack
/// @param text UTF-8 text
/// @param pixel_size pixels per em, 1 to max_pixel_size; others are clamped
/// @param bold whether the bold face goes first
/// @return the line; not drawn for text the stack refuses
[[nodiscard]] TextLine draw_line(
    oa::platform::text_font::FontStack& fonts, std::string_view text, int pixel_size, bool bold
);

/// Returns how wide a line of text is drawn: the pen's advance.
///
/// @param fonts the font stack
/// @param text UTF-8 text
/// @param pixel_size pixels per em
/// @param bold whether the bold face goes first
/// @return pixels; 0 for text the stack refuses
[[nodiscard]] int text_width(
    oa::platform::text_font::FontStack& fonts, std::string_view text, int pixel_size, bool bold
);

/// Returns the rows a line takes above and below its baseline.
///
/// @param fonts the font stack
/// @param pixel_size pixels per em
/// @param bold whether the bold face goes first
/// @return the metrics; zeros for a size the stack refuses
[[nodiscard]] oa::platform::text_font::LineMetrics
line_metrics(oa::platform::text_font::FontStack& fonts, int pixel_size, bool bold);

/// Shortens text that is wider than a width: characters are dropped from
/// its end and an ellipsis takes their place.
///
/// @param fonts the font stack
/// @param text UTF-8 text
/// @param pixel_size pixels per em
/// @param bold whether the bold face goes first
/// @param max_width the widest the line may be, in pixels
/// @return the text itself when it fits, else its longest start that fits
///     with the ellipsis; empty when not even the ellipsis fits
[[nodiscard]] std::string fit_text(
    oa::platform::text_font::FontStack& fonts,
    std::string_view text,
    int pixel_size,
    bool bold,
    int max_width
);

/// Returns an RGBA picture, its alpha straight, filtered to another size as
/// Painter::draw_rgba filters it, so that drawing the result at its own size
/// lays the same pixels as drawing the picture scaled. A pixel nothing of the
/// picture covers is clear: all four bytes 0.
///
/// @param rgba the picture, four bytes a pixel, rows top to bottom
/// @param width the picture's width in pixels
/// @param height the picture's height in pixels
/// @param columns the width it is filtered to, in pixels
/// @param rows the height it is filtered to, in pixels
/// @return columns × rows pixels, four bytes each; empty when either picture
///     has no pixels or rgba is shorter than width × height pixels
[[nodiscard]] std::vector<uint8_t>
resized_rgba(std::span<const uint8_t> rgba, int width, int height, int columns, int rows);

/// Blends a drawn line with its pen at a place on its baseline.
///
/// @param painter the painter
/// @param line the drawn line
/// @param pen_x the column the pen starts at
/// @param baseline_y the row of the baseline
/// @param colour the text's colour
/// @return the box the line covers on the canvas
Box paint_line(Painter& painter, const TextLine& line, int pen_x, int baseline_y, Rgba colour);

} // namespace oa::ui::paint
