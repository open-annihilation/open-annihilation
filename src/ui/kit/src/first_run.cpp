// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The first-run pieces, drawn through the canvas. A carousel card is drawn
// flat into the reused offscreen buffer and copied on as a trapezoid. Nothing
// here reads a preference or looks a word up.

#include "oa/ui/kit/first_run.hpp"

#include "oa/ui/kit/chrome.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/controls.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/kit/theme.hpp"

#include "oa/ui/frontend_renderer/artless.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::ui::kit {

namespace renderer = oa::ui::frontend_renderer;
namespace text_font = oa::platform::text_font;

namespace {

/// Appends a button control. No item is added.
///
/// @param[in,out] list the display list
/// @param rect where a press lands
/// @param id the control's number
/// @param name its automation name
/// @param steps true when Left and Right act on it
/// @param checked true when it is the chosen one
/// @param text its words, for automation
/// @param enabled false while a press does not reach it
void add_press(
    DisplayList& list,
    const Rect& rect,
    ControlId id,
    std::string name,
    bool steps,
    bool checked,
    std::string text,
    bool enabled
) {
    if (id == no_control)
        return;
    Control control;
    control.id = id;
    control.rect = rect;
    control.enabled = enabled;
    control.name = std::move(name);
    control.kind = ControlKind::button;
    control.steps = steps;
    control.checked = checked;
    control.text = std::move(text);
    list.controls.push_back(std::move(control));
}

/// Mixes two knot values. abs_milli is held to the first and last knot.
///
/// @param abs_milli how far along, in thousandths
/// @param knots how many knots
/// @param at each knot's place, in thousandths, ascending
/// @param value each knot's value
/// @return the mixed value
int32_t mix_knots(int32_t abs_milli, int knots, const int32_t* at, const int32_t* value) noexcept {
    if (knots <= 0)
        return 0;
    if (abs_milli <= at[0])
        return value[0];
    for (int place = 1; place < knots; ++place) {
        if (abs_milli <= at[place]) {
            const int32_t span = at[place] - at[place - 1];
            if (span <= 0)
                return value[place];
            const int32_t dist = abs_milli - at[place - 1];
            return value[place - 1] + (value[place] - value[place - 1]) * dist / span;
        }
    }
    return value[knots - 1];
}

/// Returns a card's opacity in 256ths at a distance from the centre.
///
/// The face stays opaque through two steps. It fades out over the third, so
/// the floor shows around a card and not through its name.
///
/// @param abs_milli the distance, in thousandths of a step
/// @return the opacity, 0 to 256
int32_t carousel_opacity(int32_t abs_milli) noexcept {
    constexpr int32_t kAt[] = {0, 1000, 2000, 3000};
    constexpr int32_t kValue[] = {256, 256, 256, 0};
    return mix_knots(abs_milli, 4, kAt, kValue);
}

/// Returns a card's size in thousandths of its full size.
///
/// @param abs_milli the distance, in thousandths of a step
/// @return the size, in thousandths
int32_t carousel_shrink(int32_t abs_milli) noexcept {
    constexpr int32_t kAt[] = {0, 1000, 2000, 3000};
    constexpr int32_t kValue[] = {1000, 860, 720, 720};
    return mix_knots(abs_milli, 4, kAt, kValue);
}

/// Returns how far a card falls, in points.
///
/// @param abs_milli the distance, in thousandths of a step
/// @return the fall, in points
int32_t carousel_fall(int32_t abs_milli) noexcept {
    constexpr int32_t kAt[] = {0, 1000, 2000, 3000};
    constexpr int32_t kValue[] = {0, 6, 12, 18};
    return mix_knots(abs_milli, 4, kAt, kValue);
}

/// Returns the pill colours a language card's availability uses.
///
/// @param availability the card's state
/// @return the pill's state
PillState pill_of(LanguageAvailability availability) noexcept {
    switch (availability) {
    case LanguageAvailability::active:
        return PillState::playing;
    case LanguageAvailability::failed:
        return PillState::problem;
    case LanguageAvailability::built_in:
    case LanguageAvailability::get:
    case LanguageAvailability::downloading:
    case LanguageAvailability::checking:
    case LanguageAvailability::offline:
        break;
    }
    return PillState::neutral;
}

/// Tells whether a card is dimmed into the panel.
///
/// @param look the card
/// @return true when it is offline or disabled
bool card_dimmed(const LanguageCardLook& look) noexcept {
    return look.disabled || look.availability == LanguageAvailability::offline;
}

/// Draws a line when the box has room.
///
/// @param canvas where it is drawn
/// @param role which font
/// @param text the text
/// @param box the box
/// @param ink the colour
/// @param align where the text sits along the box
void line_in(
    const Canvas& canvas,
    FontRole role,
    std::string_view text,
    const Rect& box,
    Colour ink,
    Align align = Align::left
) {
    if (box.width <= 0 || box.height <= 0 || text.empty())
        return;
    box_text(canvas, role, text, box, align, ink, 0);
}

/// The design's language card, in points, at Large. Every measure on a card
/// scales from these to the rectangle being drawn.
constexpr int32_t kDesignCardWidth = 232;
constexpr int32_t kDesignCardHeight = 304;
constexpr int32_t kDesignPadTop = 30;
constexpr int32_t kDesignPadSide = 18;
constexpr int32_t kDesignPadBottom = 22;
constexpr int32_t kDesignBadge = 88;
constexpr int32_t kDesignNameGap = 10;
constexpr int32_t kDesignNamePoints = 27;
/// Letters in the badge, in points, on the design's 88-point badge.
constexpr int32_t kDesignLetterPoints = 32;
/// A small line on the design card, and the gap under the own name and
/// between those lines. The box is at least the small font; the gap is what
/// keeps a side card's lines apart after the carousel shrinks the card.
constexpr int32_t kDesignSmallLine = 18;
constexpr int32_t kDesignLineGap = 8;

/// Scales a design measure to a rectangle's side.
///
/// @param design the measure on the design card
/// @param have the rectangle's side, in points
/// @param of the design card's side
/// @return the measure, in points; 0 when a side is empty
int32_t scale_measure(int32_t design, int32_t have, int32_t of) noexcept {
    if (design <= 0 || have <= 0 || of <= 0)
        return 0;
    return design * have / of;
}

/// The padding, badge and name size of one card.
struct CardMeasures {
    int32_t pad_top{};       ///< points above the badge
    int32_t pad_side{};      ///< points at each side
    int32_t pad_bottom{};    ///< points under the pill
    int32_t badge{};         ///< the badge's side, in points
    int32_t name_gap{};      ///< points between the badge and the own name
    int32_t name_points{};   ///< the own name's size, in points, before it shrinks
    int32_t letter_points{}; ///< the badge letters' size, in points, before they shrink
    int32_t small_line{};    ///< a small line's box, at least the small font
    int32_t line_gap{};      ///< points between the own name and the small lines
};

/// Returns a card's measures scaled from the design card.
///
/// @param rect the card
/// @return the measures; a badge no larger than the padded rectangle
CardMeasures card_measures(const Rect& rect) noexcept {
    CardMeasures measures;
    measures.pad_top = scale_measure(kDesignPadTop, rect.height, kDesignCardHeight);
    measures.pad_side = scale_measure(kDesignPadSide, rect.width, kDesignCardWidth);
    measures.pad_bottom = scale_measure(kDesignPadBottom, rect.height, kDesignCardHeight);
    measures.badge = scale_measure(kDesignBadge, rect.width, kDesignCardWidth);
    measures.name_gap = scale_measure(kDesignNameGap, rect.height, kDesignCardHeight);
    measures.name_points =
        std::max(scale_measure(kDesignNamePoints, rect.height, kDesignCardHeight), int32_t{1});
    measures.letter_points =
        std::max(scale_measure(kDesignLetterPoints, measures.badge, kDesignBadge), int32_t{1});
    measures.small_line = std::max(
        scale_measure(kDesignSmallLine, rect.height, kDesignCardHeight), compact_metrics.small_line
    );
    measures.line_gap =
        std::max(scale_measure(kDesignLineGap, rect.height, kDesignCardHeight), int32_t{2});
    const int32_t room_w = std::max(rect.width - 2 * measures.pad_side, int32_t{0});
    const int32_t room_h =
        std::max(rect.height - measures.pad_top - measures.pad_bottom, int32_t{0});
    measures.badge = std::min(measures.badge, std::min(room_w, room_h));
    if (measures.badge < 0)
        measures.badge = 0;
    return measures;
}

/// A line drawn in the modern fonts at one pixel size, the endonym face
/// included at that size.
struct EndonymLine {
    text_font::Coverage coverage{}; ///< the drawn line
    bool drawn{};                   ///< false when the fonts refused the text
};

/// Draws a line so a name the sans faces lack comes from the endonym face
/// at the same pixel size.
///
/// @param fonts the modern fonts
/// @param text the text, in UTF-8
/// @param pixel_size pixels per em
/// @param family the faces looked in first: Barlow in the modern look
/// @return the line
EndonymLine draw_endonym(
    text_font::FontStack& fonts, std::string_view text, int32_t pixel_size, text_font::Family family
) {
    text_font::Style style;
    style.pixel_size = std::clamp(pixel_size, int32_t{1}, text_font::max_pixel_size);
    style.weight = text_font::Weight::bold;
    style.rendering = text_font::Rendering::antialiased;
    style.least_cjk_pixel_size = style.pixel_size;
    style.family = family;
    EndonymLine line;
    if (std::optional<text_font::Coverage> coverage = fonts.draw(text, style)) {
        line.coverage = std::move(*coverage);
        line.drawn = true;
    }
    return line;
}

/// Returns how many rows a bold capital H stands, at a pixel size.
///
/// @param fonts the modern fonts
/// @param pixel_size pixels per em
/// @param family the faces looked in first
/// @return the rows; a share of the size when the fonts cannot draw it
int32_t
endonym_capitals(text_font::FontStack& fonts, int32_t pixel_size, text_font::Family family) {
    const EndonymLine line = draw_endonym(fonts, "H", pixel_size, family);
    const text_font::Coverage& drawn = line.coverage;
    if (!line.drawn || drawn.width <= 0)
        return std::max(pixel_size * 7 / 10, int32_t{1});
    constexpr uint8_t half_covered = 128;
    for (int32_t row = 0; row < drawn.height && row < drawn.baseline; ++row) {
        for (int32_t column = 0; column < drawn.width; ++column) {
            const std::size_t at =
                static_cast<std::size_t>(row) * static_cast<std::size_t>(drawn.width) +
                static_cast<std::size_t>(column);
            if (drawn.alpha[at] >= half_covered)
                return drawn.baseline - row;
        }
    }
    return std::max(pixel_size * 7 / 10, int32_t{1});
}

/// Returns a rectangle of a smooth canvas as the painter's pixels.
///
/// @param canvas the canvas, smooth
/// @param box the rectangle, in points
/// @return the pixels
oa::ui::paint::Box smooth_pixels(const Canvas& canvas, const Rect& box) noexcept {
    const int32_t percent = std::max(canvas.smooth.scale_percent, int32_t{1});
    const Rect drawn = to_canvas(
        {canvas.smooth.origin.x + box.x, canvas.smooth.origin.y + box.y, box.width, box.height},
        percent
    );
    return {drawn.x, drawn.y, drawn.width, drawn.height};
}

/// Draws a centred line in the modern fonts at the size asked for. A line
/// wider than the box is recorded and still drawn at that size.
///
/// @param canvas the canvas, smooth, with fonts
/// @param text the text
/// @param box the box, in points
/// @param ink the colour
/// @param points the size to start from, in points
void draw_modern_line(
    const Canvas& canvas, std::string_view text, const Rect& box, Colour ink, int32_t points
) {
    oa::ui::paint::Painter* painter = canvas.smooth.painter;
    text_font::FontStack* fonts = canvas.smooth.fonts;
    if (painter == nullptr || fonts == nullptr || text.empty() || box.width <= 0 || box.height <= 0)
        return;
    if (!canvas.smooth.clip) {
        painter->reset_clip();
    } else if (canvas.smooth.clip->empty()) {
        return;
    } else {
        painter->set_clip(*canvas.smooth.clip);
    }
    const oa::ui::paint::Box pixels = smooth_pixels(canvas, box);
    if (pixels.width <= 0 || pixels.height <= 0)
        return;
    const int32_t percent = std::max(canvas.smooth.scale_percent, int32_t{1});
    const int32_t wanted = std::clamp(
        to_canvas({0, 0, std::max(points, int32_t{1}), 0}, percent).width,
        int32_t{1},
        text_font::max_pixel_size
    );
    const int32_t size = std::clamp(wanted, int32_t{1}, text_font::max_pixel_size);
    // The modern look draws the name in Barlow Bold, a name in another
    // alphabet in the faces after it.
    const text_font::Family family =
        canvas.fonts != nullptr && modern_look(*canvas.fonts) ? text_font::Family::interface : text_font::Family::sans;
    EndonymLine line = draw_endonym(*fonts, text, size, family);
    if (!line.drawn)
        return;
    const int32_t capitals_tall = endonym_capitals(*fonts, size, family);
    const int32_t pen = pixels.x + (pixels.width - line.coverage.advance) / 2;
    // A line the large size cannot fit wraps at the font's own size. The
    // point box is the one the caller laid out.
    if (line.coverage.advance > pixels.width) {
        const Measure measure = [&](std::string_view words) {
            const int32_t measured = measure_text(canvas, FontRole::regular, words);
            return measured > 0 ? measured : estimated_width(words);
        };
        std::vector<std::string> lines = wrap(text, std::max(box.width, int32_t{1}), measure);
        if (lines.empty())
            lines.emplace_back(text);
        const int32_t step = compact_metrics.regular_line;
        const int32_t block = step * static_cast<int32_t>(lines.size());
        int32_t y = box.y + std::max((box.height - block) / 2, int32_t{0});
        for (const std::string& wrapped : lines) {
            box_text(
                canvas,
                FontRole::regular,
                wrapped,
                {box.x, y, box.width, step},
                Align::centre,
                ink,
                0
            );
            y += step;
        }
        return;
    }
    note_text_fit(
        text, line.coverage.advance, pixels.x, pixels.y, pixels.width, pixels.height, pen
    );
    const int32_t baseline = pixels.y + (pixels.height - capitals_tall) / 2 + capitals_tall;
    oa::ui::paint::TextLine painted;
    painted.coverage = std::move(line.coverage);
    painted.drawn = true;
    oa::ui::paint::paint_line(
        *painter, painted, pen, baseline, oa::ui::paint::Rgba{ink.r, ink.g, ink.b, ink.a}
    );
}

/// Draws a centred line in a game font, grown to the box's height. A line
/// wider than the box is recorded and still drawn at that height.
///
/// @param canvas the canvas, crisp
/// @param role which font
/// @param text the text
/// @param box the box, in points
/// @param ink the colour
void draw_bitmap_line(
    const Canvas& canvas, FontRole role, std::string_view text, const Rect& box, Colour ink
) {
    if (canvas.surface == nullptr || canvas.fonts == nullptr || text.empty() || box.width <= 0 ||
        box.height <= 0)
        return;
    const int32_t full = text_width(*canvas.fonts, role, text);
    const int32_t rows = std::max(capitals(*canvas.fonts, role), int32_t{1});
    if (full <= 0)
        return;
    constexpr int32_t kMostSource = 4096;
    const std::string shown(text);
    int32_t src_w = full;
    if (src_w > kMostSource)
        return;
    const int32_t src_h = rows + 2;
    renderer::Surface source;
    source.width = static_cast<uint32_t>(src_w);
    source.height = static_cast<uint32_t>(src_h);
    source.rgb.assign(
        static_cast<std::size_t>(src_w) * static_cast<std::size_t>(src_h) * 3, uint8_t{0}
    );
    renderer::Placement place;
    place.scale = 1;
    const int32_t pen_row = (src_h - rows) / 2 - 1;
    draw_text(source, place, *canvas.fonts, role, shown, 0, pen_row, rgb(ink));

    int32_t left = src_w;
    int32_t top = src_h;
    int32_t right = 0;
    int32_t bottom = 0;
    for (int32_t y = 0; y < src_h; ++y) {
        for (int32_t x = 0; x < src_w; ++x) {
            const std::size_t at = (static_cast<std::size_t>(y) * static_cast<std::size_t>(src_w) +
                                    static_cast<std::size_t>(x)) *
                                   3;
            if ((source.rgb[at] | source.rgb[at + 1] | source.rgb[at + 2]) == 0)
                continue;
            if (x < left)
                left = x;
            if (y < top)
                top = y;
            if (x + 1 > right)
                right = x + 1;
            if (y + 1 > bottom)
                bottom = y + 1;
        }
    }
    if (right <= left || bottom <= top)
        return;
    const int32_t crop_w = right - left;
    const int32_t crop_h = bottom - top;
    std::vector<uint8_t> pixels(
        static_cast<std::size_t>(crop_w) * static_cast<std::size_t>(crop_h) * 4, uint8_t{0}
    );
    for (int32_t y = 0; y < crop_h; ++y) {
        for (int32_t x = 0; x < crop_w; ++x) {
            const std::size_t from =
                (static_cast<std::size_t>(top + y) * static_cast<std::size_t>(src_w) +
                 static_cast<std::size_t>(left + x)) *
                3;
            if ((source.rgb[from] | source.rgb[from + 1] | source.rgb[from + 2]) == 0)
                continue;
            const std::size_t to = (static_cast<std::size_t>(y) * static_cast<std::size_t>(crop_w) +
                                    static_cast<std::size_t>(x)) *
                                   4;
            pixels[to] = source.rgb[from];
            pixels[to + 1] = source.rgb[from + 1];
            pixels[to + 2] = source.rgb[from + 2];
            pixels[to + 3] = 255;
        }
    }
    // The box is the design size. The line grows to that height while the
    // grown line fits the box. When it does not, the words stay at the font's
    // own size and wrap inside the box.
    int32_t dest_h = std::max(box.height, int32_t{1});
    int32_t dest_w = std::max(crop_w * dest_h / std::max(crop_h, int32_t{1}), int32_t{1});
    if (dest_w > box.width) {
        const Measure measure = [&](std::string_view words) {
            return text_width(*canvas.fonts, role, words);
        };
        std::vector<std::string> lines = wrap(shown, std::max(box.width, int32_t{1}), measure);
        if (lines.empty())
            lines.emplace_back(shown);
        const int32_t step =
            role == FontRole::regular ? compact_metrics.regular_line : compact_metrics.small_line;
        const int32_t block = step * static_cast<int32_t>(lines.size());
        int32_t y = box.y + std::max((box.height - block) / 2, int32_t{0});
        for (const std::string& line : lines) {
            box_text(canvas, role, line, {box.x, y, box.width, step}, Align::centre, ink, 0);
            y += step;
        }
        return;
    }
    const int32_t scale = std::max(canvas.placement.scale, int32_t{1});
    const int32_t origin_x = canvas.placement.scale > 0 ? canvas.placement.x / scale : 0;
    const int32_t origin_y = canvas.placement.scale > 0 ? canvas.placement.y / scale : 0;
    const int32_t ink_x = box.x + (box.width - dest_w) / 2;
    const Rect& clip = canvas.placement.clip;
    if (canvas.placement.scale == 0)
        note_text_fit(
            shown,
            dest_w,
            box.x + origin_x,
            box.y + origin_y,
            box.width,
            dest_h,
            ink_x + origin_x,
            0,
            0,
            -1,
            0
        );
    else if (clip.width > 0 && clip.height > 0)
        note_text_fit(
            shown,
            dest_w,
            box.x + origin_x,
            box.y + origin_y,
            box.width,
            dest_h,
            ink_x + origin_x,
            clip.x + origin_x,
            clip.y + origin_y,
            clip.width,
            clip.height
        );
    else
        note_text_fit(
            shown, dest_w, box.x + origin_x, box.y + origin_y, box.width, dest_h, ink_x + origin_x
        );
    const Rect dest{
        box.x + (box.width - dest_w) / 2,
        box.y + (box.height - dest_h) / 2,
        dest_w,
        dest_h,
    };
    const renderer::RgbaPicture picture{
        static_cast<uint32_t>(crop_w), static_cast<uint32_t>(crop_h), pixels
    };
    draw_picture(canvas, dest, picture);
}

/// Draws a centred line: large in the modern fonts when they are open, and
/// in a game font otherwise. A line wider than the box keeps its size.
///
/// @param canvas where it is drawn
/// @param role the game font, when the modern fonts are not open
/// @param text the text
/// @param box the box
/// @param ink the colour
/// @param modern_points the modern size to start from, in points
void draw_fitted(
    const Canvas& canvas,
    FontRole role,
    std::string_view text,
    const Rect& box,
    Colour ink,
    int32_t modern_points
) {
    if (canvas.smooth.painter != nullptr && canvas.smooth.fonts != nullptr) {
        draw_modern_line(canvas, text, box, ink, modern_points);
        return;
    }
    draw_bitmap_line(canvas, role, text, box, ink);
}

/// Writes one place of a step line, or leaves a place this line does not use.
///
/// @param[in,out] result the line so far
/// @param pattern the pattern
/// @param open the '{'
/// @param close the '}'
/// @param step the step's name
/// @param index the step, as text
/// @param count how many steps, as text
void write_place(
    std::string& result,
    std::string_view pattern,
    std::size_t open,
    std::size_t close,
    std::string_view step,
    std::string_view index,
    std::string_view count
) {
    const std::string_view name = pattern.substr(open + 1, close - open - 1);
    if (name == "step")
        result.append(step);
    else if (name == "index")
        result.append(index);
    else if (name == "count")
        result.append(count);
    else
        result.append(pattern.substr(open, close - open + 1));
}

/// Tells whether the game font has a glyph to measure. A modern-only font does not.
///
/// @param fonts the fonts
/// @return true when the regular font drew a glyph
bool game_font_has_glyphs(const Fonts& fonts);

/// Returns a button's width: button_width, or the caption with its insets.
///
/// Fonts with no glyphs, and a null font, estimate the caption. The setup's
/// smooth form draws that way, and the hit target is measured with no font,
/// so the two stay the same width. A caption such as "Copy the game" is wider
/// than button_width and would otherwise run off the window's edge.
///
/// @param fonts the fonts; null, or fonts with no glyphs, estimates the caption
/// @param caption the button's words
/// @return the width, in points
int32_t setup_button_width(const Fonts* fonts, std::string_view caption) {
    int32_t text = 0;
    if (fonts != nullptr && game_font_has_glyphs(*fonts))
        text = text_width(*fonts, FontRole::small, caption);
    else
        text = estimated_width(caption);
    return std::max(compact_metrics.button_width, text + 2 * compact_metrics.choice_text_inset);
}

/// Holds a rectangle inside a room, shrinking it when it is larger.
///
/// @param card the rectangle
/// @param room the room
/// @return the held rectangle
Rect held_inside(Rect card, const Rect& room) noexcept {
    if (room.width <= 0 || room.height <= 0)
        return {};
    if (card.width > room.width)
        card.width = room.width;
    if (card.height > room.height)
        card.height = room.height;
    if (card.x < room.x)
        card.x = room.x;
    if (card.y < room.y)
        card.y = room.y;
    if (card.x + card.width > room.x + room.width)
        card.x = room.x + room.width - card.width;
    if (card.y + card.height > room.y + room.height)
        card.y = room.y + room.height - card.height;
    return card;
}

/// Tells whether two rectangles share any area.
///
/// @param a a rectangle
/// @param b another
/// @return true when they share a point
bool shares_area(const Rect& a, const Rect& b) noexcept {
    const Rect shared = intersect(a, b);
    return shared.width > 0 && shared.height > 0;
}

/// Draws a one-point frame blended by an opacity.
///
/// @param canvas where it is drawn
/// @param rect the frame's outside
/// @param ink the colour
/// @param opacity the share, in 256ths
void blend_frame(const Canvas& canvas, const Rect& rect, Colour ink, uint32_t opacity) {
    if (rect.width <= 0 || rect.height <= 0 || opacity == 0)
        return;
    blend_rect(canvas, {rect.x, rect.y, rect.width, 1}, ink, opacity);
    if (rect.height > 1)
        blend_rect(canvas, {rect.x, rect.y + rect.height - 1, rect.width, 1}, ink, opacity);
    if (rect.height <= 2)
        return;
    blend_rect(canvas, {rect.x, rect.y + 1, 1, rect.height - 2}, ink, opacity);
    if (rect.width > 1)
        blend_rect(canvas, {rect.x + rect.width - 1, rect.y + 1, 1, rect.height - 2}, ink, opacity);
}

/// Draws the accent line from a coach card to its target.
///
/// @param canvas where it is drawn
/// @param card the card
/// @param target the target
void draw_connector(const Canvas& canvas, const Rect& card, const Rect& target) {
    if (target.width <= 0 || target.height <= 0)
        return;
    const int32_t card_x = card.x + card.width / 2;
    const int32_t card_y = card.y + card.height / 2;
    const int32_t target_x = target.x + target.width / 2;
    const int32_t target_y = target.y + target.height / 2;
    const int32_t dx = target_x - card_x;
    const int32_t dy = target_y - card_y;
    if (std::abs(dx) >= std::abs(dy)) {
        const int32_t y = std::clamp(target_y, card.y, std::max(card.y + card.height - 1, card.y));
        const int32_t from = dx >= 0 ? card.x + card.width : target.x + target.width;
        const int32_t to = dx >= 0 ? target.x : card.x;
        if (to > from)
            rule_rect(canvas, {from, y, to - from, 1}, colour::accent);
        return;
    }
    const int32_t x = std::clamp(target_x, card.x, std::max(card.x + card.width - 1, card.x));
    const int32_t from = dy >= 0 ? card.y + card.height : target.y + target.height;
    const int32_t to = dy >= 0 ? target.y : card.y;
    if (to > from)
        rule_rect(canvas, {x, from, 1, to - from}, colour::accent);
}

/// A smooth line's width, in points, with the same tracking the header draws.
///
/// @param canvas a smooth canvas
/// @param role which font
/// @param text the line
/// @param tracking extra points after each character but the last
/// @return the width; 0 when the modern fonts are not open
int32_t
smooth_tracked(const Canvas& canvas, FontRole role, std::string_view text, int32_t tracking) {
    if (text.empty() || canvas.smooth.fonts == nullptr)
        return 0;
    // The modern look measures in points at every scale.
    if (canvas.fonts != nullptr && modern_look(*canvas.fonts))
        return tracked_width(*canvas.fonts, role, text, tracking);
    if (tracking == 0)
        return measure_text(canvas, role, text);
    int32_t width = 0;
    bool any = false;
    for (std::size_t at = 0; at < text.size();) {
        const std::size_t bytes = character_bytes(text.substr(at));
        if (bytes == 0)
            break;
        if (any)
            width += tracking;
        width += measure_text(canvas, role, text.substr(at, bytes));
        any = true;
        at += bytes;
    }
    return width;
}

bool game_font_has_glyphs(const Fonts& fonts) {
    return std::any_of(
        fonts.regular.font.glyphs.begin(), fonts.regular.font.glyphs.end(), [](const auto& glyph) {
            return glyph.has_value();
        }
    );
}

/// The card nearest a centre, wrapping past either end.
///
/// Halfway rounds the same way a non-negative centre does. A centre of -1000
/// on five cards is the last card, and one of count times 1000 is the first.
///
/// @param centre_milli where the centre sits, in thousandths of an index
/// @param count how many cards
/// @return the card, from 0
int32_t wrapped_card(int32_t centre_milli, int32_t count) noexcept {
    if (count <= 0)
        return 0;
    const int64_t shifted = static_cast<int64_t>(centre_milli) + 500;
    const int64_t rounded = shifted >= 0 ? shifted / 1000 : (shifted - 999) / 1000;
    int32_t index = static_cast<int32_t>(rounded % count);
    if (index < 0)
        index += count;
    return index;
}

} // namespace

std::string step_line(const StepLook& look) {
    const std::string_view pattern = look.pattern.empty() ? step_line_pattern : look.pattern;
    const std::string index = std::to_string(std::max(look.index, int32_t{0}));
    const std::string count = std::to_string(std::max(look.count, int32_t{0}));
    std::string result;
    std::size_t at = 0;
    while (at < pattern.size()) {
        const std::size_t open = pattern.find('{', at);
        if (open == std::string_view::npos) {
            result.append(pattern.substr(at));
            break;
        }
        const std::size_t close = pattern.find('}', open);
        if (close == std::string_view::npos) {
            result.append(pattern.substr(at));
            break;
        }
        result.append(pattern.substr(at, open - at));
        write_place(result, pattern, open, close, look.name, index, count);
        at = close + 1;
    }
    return result;
}

void carousel_go(CarouselModel& model, int32_t index) noexcept {
    if (model.count <= 1) {
        model.selected = 0;
        model.origin_milli = model.count <= 0 ? 0 : carousel_index_milli(model);
        model.elapsed_ms = 0;
        return;
    }
    int32_t end = index % model.count;
    if (end < 0)
        end += model.count;
    const int32_t current = carousel_index_milli(model);
    const int32_t span = model.count * 1000;
    const int32_t asked = index * 1000;
    int32_t best_dest = end * 1000;
    int32_t best_distance = std::abs(best_dest - current);
    for (int lap = -2; lap <= 2; ++lap) {
        const int32_t dest = end * 1000 + lap * span;
        const int32_t distance = std::abs(dest - current);
        if (distance < best_distance) {
            best_dest = dest;
            best_distance = distance;
        } else if (
            distance == best_distance && distance > 0 && (asked > current) == (dest > current) &&
            (asked > current) != (best_dest > current)
        ) {
            best_dest = dest;
        }
    }
    const int32_t laps = (best_dest - end * 1000) / span;
    model.origin_milli = current - laps * span;
    model.selected = end;
    model.elapsed_ms = 0;
}

void carousel_tick(CarouselModel& model, uint32_t dt_ms) noexcept {
    if (model.elapsed_ms >= carousel_move_ms)
        return;
    const uint32_t room = carousel_move_ms - model.elapsed_ms;
    model.elapsed_ms += dt_ms < room ? dt_ms : room;
}

void carousel_settle(CarouselModel& model) noexcept {
    if (model.count <= 0) {
        model.selected = 0;
        model.origin_milli = 0;
        model.elapsed_ms = carousel_move_ms;
        return;
    }
    if (model.selected < 0)
        model.selected = 0;
    if (model.selected >= model.count)
        model.selected = model.count - 1;
    model.origin_milli = model.selected * 1000;
    model.elapsed_ms = carousel_move_ms;
}

int32_t carousel_index_milli(const CarouselModel& model) noexcept {
    if (model.count <= 0)
        return 0;
    const int32_t target = std::clamp(model.selected, int32_t{0}, model.count - 1) * 1000;
    return carousel_centre_milli(model.origin_milli, target, model.elapsed_ms);
}

CarouselGeometry carousel_fit(int32_t room, const Metrics& metrics) noexcept {
    const int32_t full_width = std::max(metrics.language_card_width, int32_t{1});
    CarouselGeometry geometry{
        full_width,
        std::max(metrics.language_card_height, int32_t{1}),
        std::max(metrics.carousel_step, int32_t{1}),
    };
    const int32_t span = 4 * geometry.step + geometry.card_width;
    if (room > 0 && span > room) {
        geometry.card_width = std::max(room / 5, int32_t{1});
        geometry.card_height =
            std::max(metrics.language_card_height * geometry.card_width / full_width, int32_t{1});
        geometry.step = geometry.card_width;
    }
    return geometry;
}

Rect carousel_band(const Rect& rect) noexcept {
    const int32_t band = std::min(carousel_pager_band, std::max(rect.height - 1, int32_t{0}));
    return {
        rect.x, rect.y, std::max(rect.width, int32_t{0}), std::max(rect.height - band, int32_t{0})
    };
}

std::vector<CarouselSlot> carousel_places(
    const Rect& area,
    const CarouselGeometry& geometry,
    int32_t count,
    int32_t centre_milli,
    bool reduced_motion,
    bool wrap
) {
    std::vector<CarouselSlot> slots;
    if (count <= 0 || area.width <= 0 || area.height <= 0 || geometry.card_width <= 0 ||
        geometry.card_height <= 0)
        return slots;
    const int32_t nearest = wrap && count > 1
                                ? wrapped_card(centre_milli, count)
                                : std::clamp((centre_milli + 500) / 1000, int32_t{0}, count - 1);
    const int32_t mid = area.x + area.width / 2;
    for (int32_t index = 0; index < count; ++index) {
        int32_t delta = index * 1000 - centre_milli;
        if (wrap && count > 1) {
            const int32_t span = count * 1000;
            const int32_t half = span / 2;
            while (delta > half)
                delta -= span;
            while (delta <= -half)
                delta += span;
        }
        const int32_t distance = std::abs(delta);
        if (distance >= 3000)
            continue;
        const int32_t shrink = reduced_motion ? 1000 : carousel_shrink(distance);
        const int32_t fall = reduced_motion ? 0 : carousel_fall(distance);
        int32_t face_w = std::max(geometry.card_width * shrink / 1000, int32_t{1});
        int32_t face_h = std::max(geometry.card_height * shrink / 1000, int32_t{1});
        // Reduced motion keeps every card at full size and centred on the
        // band, even when the card is taller than the band. Clamping it
        // into the band would shrink it and drop it, which flat motion
        // does not do. A turned card still stays inside the band.
        if (!reduced_motion) {
            if (face_w > area.width)
                face_w = area.width;
            if (face_h > area.height)
                face_h = area.height;
        }
        const int32_t shift =
            static_cast<int32_t>((static_cast<int64_t>(delta) * geometry.step) / 1000);
        const int32_t left = mid + shift - face_w / 2;
        int32_t top = area.y + (area.height - face_h) / 2 + fall;
        if (!reduced_motion) {
            if (top < area.y)
                top = area.y;
            if (top + face_h > area.y + area.height)
                top = area.y + area.height - face_h;
        }
        CarouselSlot slot;
        slot.index = index;
        slot.face = {left, top, face_w, face_h};
        slot.degrees = reduced_motion ? 0 : delta * carousel_turn_degrees / 1000;
        slot.opacity = carousel_opacity(distance);
        slot.centre = index == nearest;
        slot.distance_milli = distance;
        slots.push_back(slot);
    }
    std::sort(slots.begin(), slots.end(), [](const CarouselSlot& a, const CarouselSlot& b) {
        if (a.distance_milli != b.distance_milli)
            return a.distance_milli > b.distance_milli;
        return a.index < b.index;
    });
    return slots;
}

CarouselCommand carousel_key(Key pressed) noexcept {
    CarouselCommand command;
    if (pressed == Key::left)
        command.previous = true;
    else if (pressed == Key::right)
        command.next = true;
    else if (pressed == Key::enter)
        command.accept = true;
    return command;
}

CarouselEvent carousel_command(CarouselModel& model, CarouselCommand command) noexcept {
    if (command.accept)
        return CarouselEvent::picked;
    const int32_t before = model.selected;
    if (command.next)
        carousel_go(model, model.selected + 1);
    else if (command.previous)
        carousel_go(model, model.selected - 1);
    else
        return CarouselEvent::none;
    return model.selected == before ? CarouselEvent::none : CarouselEvent::moved;
}

CarouselEvent carousel_pointer(
    CarouselModel& model,
    const std::vector<CarouselSlot>& places,
    const Rect& previous,
    const Rect& next,
    CarouselDrag& drag,
    PointerPhase phase,
    Point at,
    uint8_t clicks
) noexcept {
    if (phase == PointerPhase::down) {
        drag = {};
        drag.tracking = true;
        drag.from = at;
        drag.at = at;
        drag.card = -1;
        if (previous.width > 0 && previous.height > 0 && contains(previous, at))
            drag.previous = true;
        else if (next.width > 0 && next.height > 0 && contains(next, at))
            drag.next = true;
        else {
            for (auto slot = places.rbegin(); slot != places.rend(); ++slot) {
                if (contains(slot->face, at)) {
                    drag.card = slot->index;
                    break;
                }
            }
        }
        return CarouselEvent::none;
    }
    if (phase == PointerPhase::move) {
        if (drag.tracking)
            drag.at = at;
        return CarouselEvent::none;
    }
    if (!drag.tracking)
        return CarouselEvent::none;
    const Point start = drag.from;
    const int32_t card = drag.card;
    const bool on_previous = drag.previous;
    const bool on_next = drag.next;
    drag = {};
    const int32_t dx = at.x - start.x;
    const int32_t dy = at.y - start.y;
    if (std::abs(dx) >= carousel_swipe_threshold && std::abs(dx) >= std::abs(dy)) {
        CarouselCommand command;
        command.previous = dx > 0;
        command.next = dx < 0;
        return carousel_command(model, command);
    }
    if (on_previous)
        return carousel_command(model, CarouselCommand{true, false, false});
    if (on_next)
        return carousel_command(model, CarouselCommand{false, true, false});
    if (card < 0)
        return CarouselEvent::none;
    if (clicks >= 2) {
        if (card != model.selected)
            carousel_go(model, card);
        return CarouselEvent::picked;
    }
    const int32_t centre =
        model.count <= 1 ? 0 : wrapped_card(carousel_index_milli(model), model.count);
    // The selected card is the one a press is bringing to the centre. A second
    // press on it picks it before that move has arrived.
    if (card == centre || card == model.selected)
        return carousel_command(model, CarouselCommand{false, false, true});
    carousel_go(model, card);
    return CarouselEvent::moved;
}

LanguageCardLayout language_card_layout(Point at, const Metrics& metrics, bool progress) noexcept {
    LanguageCardLayout laid;
    laid.card = {at.x, at.y, metrics.language_card_width, metrics.language_card_height};
    if (progress) {
        laid.progress = {
            at.x,
            at.y + metrics.language_card_height + language_progress_gap,
            metrics.language_card_width,
            metrics.progress_bar_height,
        };
    }
    return laid;
}

void draw_prominent_line(const Canvas& canvas, std::string_view text, const Rect& box, Colour ink) {
    draw_fitted(canvas, FontRole::regular, text, box, ink, 32);
}

void draw_language_card(const Canvas& canvas, const Rect& rect, const LanguageCardLook& look) {
    if (!can_draw(canvas) || rect.width <= 0 || rect.height <= 0)
        return;
    const bool active = look.availability == LanguageAvailability::active;
    // The ring sits one point outside the card, so it is drawn before the
    // card clips itself. A carousel copies the card from a buffer of the
    // card's own size and draws the centre card's ring afterwards.
    if (active && !look.disabled)
        outline_rect(canvas, grown(rect, 1), colour::accent);
    Canvas local = placed_at(clipped(canvas, rect), {rect.x, rect.y});
    const Rect face{0, 0, rect.width, rect.height};
    Colour ground = colour::panel;
    if (!look.disabled && look.held)
        ground = colour::list;
    else if (!look.disabled && look.hovered)
        ground = colour::hover;
    fill_rect(local, face, ground);
    outline_rect(local, face, active && !look.disabled ? colour::accent : colour::control_border);

    const CardMeasures measures = card_measures(rect);
    const int32_t content_w = std::max(rect.width - 2 * measures.pad_side, int32_t{0});
    const int32_t pill_h = std::min(
        pill_height(), std::max(rect.height - measures.pad_top - measures.pad_bottom, int32_t{0})
    );
    const int32_t pill_top = rect.height - measures.pad_bottom - pill_h;
    int32_t badge = measures.badge;
    const int32_t name_h = std::max(measures.name_points, int32_t{1});
    if (measures.pad_top + badge + measures.name_gap + name_h > pill_top)
        badge = std::max(pill_top - measures.pad_top - measures.name_gap - name_h, int32_t{0});
    badge = std::min(badge, content_w);
    const Rect badge_rect{
        measures.pad_side + std::max(content_w - badge, int32_t{0}) / 2,
        measures.pad_top,
        std::max(badge, int32_t{0}),
        std::max(badge, int32_t{0}),
    };
    if (badge > 0) {
        if (renderer::picture_drawable(look.badge)) {
            draw_picture(local, badge_rect, look.badge);
        } else {
            fill_rect(
                local, badge_rect, active && !look.disabled ? colour::accent : colour::control_hover
            );
            const int32_t letter_h =
                std::min(std::max(measures.letter_points, int32_t{1}), badge_rect.height);
            const Rect letter_box{
                badge_rect.x,
                badge_rect.y + (badge_rect.height - letter_h) / 2,
                badge_rect.width,
                letter_h,
            };
            draw_fitted(
                local,
                FontRole::regular,
                look.letters,
                letter_box,
                active && !look.disabled ? colour::on_accent : colour::text,
                measures.letter_points
            );
        }
    }

    const Colour ink = look.disabled ? colour::switch_idle : colour::text;
    const Colour hint = look.disabled ? colour::switch_idle : colour::hint;
    const Colour quiet = look.disabled ? colour::switch_idle : colour::quiet;
    int32_t text_y = measures.pad_top + badge + measures.name_gap;
    const int32_t small = rect.height < 64 ? 8 : measures.small_line;
    const int32_t line_gap = rect.height < 64 ? 2 : measures.line_gap;
    if (text_y + name_h <= pill_top && content_w > 0) {
        draw_fitted(
            local,
            FontRole::regular,
            look.own_name,
            {measures.pad_side, text_y, content_w, name_h},
            ink,
            measures.name_points
        );
        text_y += name_h + line_gap;
    }
    if (text_y + small <= pill_top && content_w > 0) {
        line_in(
            local,
            FontRole::small,
            look.english_name,
            {measures.pad_side, text_y, content_w, small},
            hint,
            Align::centre
        );
        text_y += small + line_gap;
    }
    // Facts stay one line when they fit. A line wider than the card wraps
    // onto the next small line, above the pill, so a narrow card still shows
    // every word. The pill is drawn under that, not over it.
    if (text_y + small <= pill_top && content_w > 0 && !look.facts.empty()) {
        const auto width_of = [&](std::string_view words) {
            const int32_t measured = measure_text(local, FontRole::small, words);
            if (measured > 0 || local.fonts != nullptr)
                return measured;
            return estimated_width(words);
        };
        if (width_of(look.facts) <= content_w) {
            line_in(
                local,
                FontRole::small,
                look.facts,
                {measures.pad_side, text_y, content_w, small},
                quiet,
                Align::centre
            );
        } else {
            const std::vector<std::string> lines = wrap(look.facts, content_w, width_of);
            int32_t y = text_y;
            for (const std::string& wrapped : lines) {
                if (y + small > pill_top)
                    break;
                line_in(
                    local,
                    FontRole::small,
                    wrapped,
                    {measures.pad_side, y, content_w, small},
                    quiet,
                    Align::centre
                );
                y += small + line_gap;
            }
        }
    }

    if (pill_h > 0 && !look.pill.empty()) {
        const PillLook pill{look.pill, pill_of(look.availability)};
        // The words set the width. A capsule only as wide as it is tall clips
        // "Active" and "Built in" when the game's fonts are not open yet.
        const int32_t pill_w = pill_width_of(local, pill);
        if (pill_w > 0 && pill_h > 0) {
            draw_pill(local, {(rect.width - pill_w) / 2, pill_top, pill_w, pill_h}, pill);
        }
    }
    if (card_dimmed(look))
        draw_locked_fade(local, face);
}

void add_language_card(
    DisplayList& list,
    const Rect& rect,
    const LanguageCardLook& look,
    ControlId id,
    std::string name
) {
    Item item;
    item.role = Role::language_card;
    item.rect = rect;
    item.control = id;
    item.text = look.own_name;
    item.state.hovered = look.hovered;
    item.state.pressed = look.held;
    item.state.disabled = look.disabled;
    item.state.selected = look.availability == LanguageAvailability::active;
    item.look = look;
    list.items.push_back(std::move(item));
    add_press(list, rect, id, std::move(name), false, false, look.own_name, !look.disabled);
}

void draw_carousel(const Canvas& canvas, const Rect& rect, const CarouselLook& look) {
    if (!can_draw(canvas) || rect.width <= 0 || rect.height <= 0)
        return;
    const Rect band = carousel_band(rect);
    const CarouselGeometry geometry{look.card_width, look.card_height, look.step};
    const auto count = static_cast<int32_t>(look.cards.size());
    const std::vector<CarouselSlot> slots =
        carousel_places(band, geometry, count, look.centre_milli, look.reduced_motion, look.wrap);
    for (const CarouselSlot& slot : slots) {
        if (slot.index < 0 || slot.index >= count || slot.opacity <= 0)
            continue;
        const CarouselCard& source = look.cards[static_cast<std::size_t>(slot.index)];
        LanguageCardLook card;
        card.letters = source.letters;
        card.own_name = source.own_name;
        card.english_name = source.english_name;
        card.facts = source.facts;
        // Two steps out the pill is copied through the turn and cannot be
        // read. The centre and the cards one step out keep theirs.
        if (slot.distance_milli < carousel_visible_side * 1000)
            card.pill = source.pill;
        card.availability = source.availability;
        card.hovered = source.hovered;
        card.held = source.held;
        card.disabled = source.disabled;
        Perspective how;
        how.degrees = static_cast<float>(slot.degrees);
        how.opacity = static_cast<float>(slot.opacity) / 256.F;
        const Rect drawn{0, 0, look.card_width, look.card_height};
        perspective_blit(canvas, drawn, slot.face, how, [&](const Canvas& offscreen) {
            draw_language_card(offscreen, drawn, card);
        });
        // The chosen card's accent border and the one-point ring outside it,
        // once it faces the player. A side card keeps the border it was drawn
        // with; its turn and its opacity come from the slot.
        const bool marked = look.chosen >= 0 ? slot.index == look.chosen : slot.centre;
        if (marked && slot.degrees == 0 && !source.disabled) {
            outline_rect(canvas, slot.face, colour::accent);
            outline_rect(canvas, grown(slot.face, 1), colour::accent);
        }
    }

    if (count <= 0)
        return;
    constexpr int32_t kBar = 14;
    constexpr int32_t kGap = 3;
    constexpr int32_t kBarHeight = 3;
    const int32_t lit = look.wrap && count > 1
                            ? wrapped_card(look.centre_milli, count)
                            : std::clamp((look.centre_milli + 500) / 1000, int32_t{0}, count - 1);
    const int32_t row_w = count * kBar + (count - 1) * kGap;
    int32_t x = rect.x + std::max(rect.width - row_w, int32_t{0}) / 2;
    const int32_t y =
        rect.y + rect.height - carousel_pager_band + (carousel_pager_band - kBarHeight) / 2;
    for (int32_t index = 0; index < count; ++index) {
        if (x >= rect.x + rect.width)
            break;
        const int32_t bar_w = std::min(kBar, rect.x + rect.width - x);
        fill_rect(
            canvas,
            {x, y, bar_w, kBarHeight},
            index == lit ? colour::accent : colour::control_border
        );
        x += kBar + kGap;
    }
}

void add_carousel(DisplayList& list, const Rect& rect, const CarouselLook& look) {
    Item item;
    item.role = Role::carousel;
    item.rect = rect;
    item.look = look;
    list.items.push_back(std::move(item));
}

void add_carousel_control(
    DisplayList& list,
    const Rect& rect,
    ControlId id,
    std::string name,
    std::string text,
    bool enabled
) {
    add_press(list, rect, id, std::move(name), true, false, std::move(text), enabled);
}

int32_t step_indicator_width(const Fonts& fonts, const StepLook& look) {
    const std::string line = step_line(look);
    const int32_t text =
        tracked_width(fonts, FontRole::small, line, compact_metrics.heading_tracking);
    const int32_t count = std::max(look.count, int32_t{0});
    const int32_t segments = count <= 0 ? 0 : count * 14 + (count - 1) * 3;
    return std::max(text, segments);
}

void draw_step_indicator(const Canvas& canvas, const Rect& rect, const StepLook& look) {
    if (!can_draw(canvas) || rect.width <= 0 || rect.height <= 0)
        return;
    const std::string line = step_line(look);
    const int32_t text_h = std::min(compact_metrics.small_line, rect.height);
    box_text(
        canvas,
        FontRole::small,
        line,
        {rect.x, rect.y, rect.width, text_h},
        Align::left,
        colour::text,
        compact_metrics.heading_tracking
    );
    const int32_t count = std::max(look.count, int32_t{0});
    if (count <= 0)
        return;
    constexpr int32_t kSegment = 14;
    constexpr int32_t kGap = 3;
    constexpr int32_t kHeight = 3;
    int32_t x = rect.x;
    const int32_t y = rect.y + text_h + 2;
    for (int32_t index = 0; index < count; ++index) {
        if (x >= rect.x + rect.width)
            break;
        const int32_t width = std::min(kSegment, rect.x + rect.width - x);
        fill_rect(
            canvas,
            {x, y, width, kHeight},
            index < look.index ? colour::accent : colour::control_border
        );
        x += kSegment + kGap;
    }
}

void add_step_indicator(DisplayList& list, const Rect& rect, const StepLook& look) {
    Item item;
    item.role = Role::step_indicator;
    item.rect = rect;
    item.text = step_line(look);
    item.look = look;
    list.items.push_back(std::move(item));
}

SetupButtons setup_buttons(const Fonts* fonts, const SetupChromeLook& look) {
    SetupButtons buttons;
    const int32_t rule_row = look.height - compact_metrics.edge - compact_metrics.footer_height - 1;
    const int32_t y =
        rule_row + 1 + (compact_metrics.footer_height - compact_metrics.button_height) / 2;
    const int32_t height = compact_metrics.button_height;
    const int32_t skip_w = setup_button_width(fonts, look.skip);
    const int32_t back_w = setup_button_width(fonts, look.back);
    const int32_t next_w = setup_button_width(fonts, look.next);
    const int32_t left = compact_metrics.edge + compact_metrics.padding;
    const int32_t right = look.width - compact_metrics.edge - compact_metrics.padding;
    buttons.skip = {left, y, skip_w, height};
    buttons.next = {right - next_w, y, next_w, height};
    buttons.back = {buttons.next.x - compact_metrics.button_gap - back_w, y, back_w, height};
    return buttons;
}

void draw_setup_chrome(const Canvas& canvas, const SetupChromeLook& look) {
    if (!can_draw(canvas) || look.width <= 0 || look.height <= 0)
        return;
    const Rect whole{0, 0, look.width, look.height};
    draw_window_face(canvas, whole);

    StepLook step;
    step.name = look.step_name;
    step.pattern = look.step_pattern;
    step.index = look.step_index;
    step.count = look.step_count;
    const bool smooth = canvas.smooth.painter != nullptr && canvas.smooth.fonts != nullptr;
    const bool modern_only = canvas.fonts == nullptr || !game_font_has_glyphs(*canvas.fonts);
    const int32_t segments =
        step.count <= 0 ? 0 : step.count * 14 + (std::max(step.count, int32_t{0}) - 1) * 3;
    int32_t step_w = 0;
    if (!modern_only && canvas.fonts != nullptr)
        step_w = step_indicator_width(*canvas.fonts, step);
    else if (smooth)
        step_w = std::max(
            smooth_tracked(
                canvas, FontRole::small, step_line(step), compact_metrics.heading_tracking
            ),
            segments
        );
    HeaderLook header;
    header.width = look.width;
    header.mark = look.brand;
    if (look.brand)
        header.title = "Open Annihilation";
    header.tracking = compact_metrics.heading_tracking;
    header.second = look.page;
    header.right_inset = step_w + compact_metrics.padding;
    if (modern_only && smooth) {
        header.title_width =
            smooth_tracked(canvas, FontRole::regular, header.title, header.tracking);
        header.second_width =
            smooth_tracked(canvas, FontRole::regular, header.second, header.tracking);
    } else if (canvas.fonts != nullptr) {
        header.title_width =
            tracked_width(*canvas.fonts, FontRole::regular, header.title, header.tracking);
        header.second_width =
            tracked_width(*canvas.fonts, FontRole::regular, header.second, header.tracking);
    }
    draw_header(canvas, header);
    if (step_w > 0) {
        const int32_t step_x = look.width - compact_metrics.edge - compact_metrics.padding - step_w;
        const int32_t step_y =
            compact_metrics.edge + (compact_metrics.header_height - step_indicator_height()) / 2;
        draw_step_indicator(canvas, {step_x, step_y, step_w, step_indicator_height()}, step);
    }

    const int32_t rule_row = look.height - compact_metrics.edge - compact_metrics.footer_height - 1;
    draw_footer_band(canvas, look.width, rule_row);
    const SetupButtons buttons = setup_buttons(canvas.fonts, look);
    draw_button(
        canvas,
        buttons.skip,
        ButtonLook{
            look.skip, ButtonStyle::quiet, look.skip_hovered, look.skip_held, look.skip_enabled
        }
    );
    draw_button(
        canvas,
        buttons.back,
        ButtonLook{
            look.back, ButtonStyle::plain, look.back_hovered, look.back_held, look.back_enabled
        }
    );
    draw_button(
        canvas,
        buttons.next,
        ButtonLook{look.next, look.next_style, look.next_hovered, look.next_held, look.next_enabled}
    );
    draw_window_edge(canvas, whole);
}

void add_setup_chrome(DisplayList& list, Point origin, const SetupChromeLook& look) {
    Item item;
    item.role = Role::setup_chrome;
    item.rect = {origin.x, origin.y, look.width, look.height};
    item.text = look.page;
    item.look = look;
    list.items.push_back(std::move(item));
}

void add_setup_controls(
    DisplayList& list,
    Point origin,
    const SetupButtons& buttons,
    const SetupChromeLook& look,
    ControlId skip_id,
    std::string skip_name,
    ControlId back_id,
    std::string back_name,
    ControlId next_id,
    std::string next_name
) {
    const auto shifted = [origin](const Rect& rect) {
        return Rect{rect.x + origin.x, rect.y + origin.y, rect.width, rect.height};
    };
    add_press(
        list,
        shifted(buttons.skip),
        skip_id,
        std::move(skip_name),
        false,
        false,
        look.skip,
        look.skip_enabled
    );
    add_press(
        list,
        shifted(buttons.back),
        back_id,
        std::move(back_name),
        false,
        false,
        look.back,
        look.back_enabled
    );
    add_press(
        list,
        shifted(buttons.next),
        next_id,
        std::move(next_name),
        false,
        false,
        look.next,
        look.next_enabled
    );
}

int32_t choice_step(int32_t index, int32_t count, Key pressed) noexcept {
    if (count <= 0)
        return 0;
    int32_t next = index;
    if (pressed == Key::up || pressed == Key::left)
        --next;
    else if (pressed == Key::down || pressed == Key::right)
        ++next;
    if (next < 0)
        next = 0;
    if (next >= count)
        next = count - 1;
    return next;
}

void draw_choice_card(const Canvas& canvas, const Rect& rect, const ChoiceCardLook& look) {
    if (!can_draw(canvas) || rect.width <= 0 || rect.height <= 0)
        return;
    Colour ground = colour::panel;
    Colour border = colour::control_border;
    if (!look.disabled && look.held)
        ground = colour::list;
    else if (!look.disabled && look.hovered)
        ground = colour::hover;
    if (!look.disabled && look.chosen)
        border = colour::accent;
    else if (!look.disabled && look.hovered)
        border = colour::control_hover;
    fill_rect(canvas, rect, ground);
    outline_rect(canvas, rect, border);

    constexpr int32_t kDisc = 10;
    const int32_t disc_x = rect.x + 8;
    const int32_t disc_y = rect.y + std::max(rect.height - kDisc, int32_t{0}) / 2;
    const Rect disc{disc_x, disc_y, kDisc, kDisc};
    const Colour radio = look.disabled ? colour::switch_idle : colour::accent;
    if (look.chosen && !look.disabled)
        fill_round_rect(canvas, disc, kDisc / 2, radio);
    else
        outline_round_rect(canvas, disc, kDisc / 2, look.disabled ? colour::switch_idle : border);

    const Colour title = look.disabled ? colour::switch_idle : colour::text;
    const Colour hint = look.disabled ? colour::switch_idle : colour::hint;
    const int32_t text_x = disc.x + disc.width + 6;
    const int32_t text_w = std::max(rect.x + rect.width - 6 - text_x, int32_t{0});
    const int32_t title_y = rect.y + 6;
    line_in(
        canvas,
        FontRole::regular,
        look.title,
        {text_x, title_y, text_w, compact_metrics.regular_line},
        title
    );
    // The hint wraps in the card. A card laid at choice_card_height holds one
    // line; a taller card holds the rest.
    const Measure measure = [&](std::string_view words) {
        const int32_t measured = measure_text(canvas, FontRole::small, words);
        if (measured > 0 || canvas.fonts != nullptr)
            return measured;
        return estimated_width(words);
    };
    std::vector<std::string> hint_lines;
    if (text_w > 0 && !look.hint.empty())
        hint_lines = wrap(look.hint, text_w, measure);
    if (hint_lines.empty() && !look.hint.empty())
        hint_lines.emplace_back(look.hint);
    int32_t hint_y = title_y + compact_metrics.regular_line + compact_metrics.hint_gap;
    for (const std::string& line : hint_lines) {
        line_in(
            canvas,
            FontRole::small,
            line,
            {text_x, hint_y, text_w, compact_metrics.small_line},
            hint
        );
        hint_y += compact_metrics.small_line;
    }
}

void add_choice_card(
    DisplayList& list, const Rect& rect, const ChoiceCardLook& look, ControlId id, std::string name
) {
    Item item;
    item.role = Role::choice_card;
    item.rect = rect;
    item.control = id;
    item.text = look.title;
    item.state.hovered = look.hovered;
    item.state.pressed = look.held;
    item.state.disabled = look.disabled;
    item.state.selected = look.chosen;
    item.look = look;
    list.items.push_back(std::move(item));
    add_press(list, rect, id, std::move(name), false, look.chosen, look.title, !look.disabled);
}

Rect place_coach_mark(
    const Rect& room, const Rect& target, int32_t card_width, int32_t card_height
) noexcept {
    const Rect right{target.x + target.width + coach_gap, target.y, card_width, card_height};
    const Rect left{target.x - coach_gap - card_width, target.y, card_width, card_height};
    const Rect below{target.x, target.y + target.height + coach_gap, card_width, card_height};
    const Rect above{target.x, target.y - coach_gap - card_height, card_width, card_height};
    const Rect sides[] = {right, left, below, above};
    for (const Rect& side : sides) {
        const Rect card = held_inside(side, room);
        if (card.width > 0 && card.height > 0 && wholly_in(card, room) &&
            !shares_area(card, target))
            return card;
    }
    return held_inside(right, room);
}

void draw_coach_mark(const Canvas& canvas, const Rect& rect, const CoachMarkLook& look) {
    if (!can_draw(canvas) || rect.width <= 0 || rect.height <= 0)
        return;
    draw_connector(canvas, rect, look.target);
    fill_rect(canvas, rect, colour::panel);
    outline_rect(canvas, rect, colour::accent);
    const std::string number = std::to_string(look.number);
    constexpr int32_t kNumber = 14;
    const Rect number_box{rect.x + 6, rect.y + 6, kNumber, kNumber};
    fill_rect(canvas, number_box, colour::accent);
    box_text(canvas, FontRole::small, number, number_box, Align::centre, colour::on_accent, 0);
    const int32_t title_x = number_box.x + number_box.width + 4;
    line_in(
        canvas,
        FontRole::regular,
        look.title,
        {title_x,
         rect.y + 4,
         std::max(rect.x + rect.width - 6 - title_x, int32_t{0}),
         compact_metrics.regular_line},
        colour::text
    );
    if (look.body.empty() || rect.width <= 12)
        return;
    const int32_t body_y = rect.y + 6 + compact_metrics.regular_line + 2;
    const int32_t body_w = std::max(rect.width - 12, int32_t{0});
    const Measure measure = [&](std::string_view words) {
        if (canvas.fonts == nullptr || words.empty())
            return estimated_width(words);
        const int32_t measured = text_width(*canvas.fonts, FontRole::small, words);
        if (measured <= 0)
            return estimated_width(words);
        return measured;
    };
    const std::vector<std::string> lines = wrap(look.body, body_w, measure);
    int32_t y = body_y;
    for (const std::string& line : lines) {
        if (y + compact_metrics.small_line > rect.y + rect.height - 4)
            break;
        line_in(
            canvas,
            FontRole::small,
            line,
            {rect.x + 6, y, body_w, compact_metrics.small_line},
            colour::hint
        );
        y += compact_metrics.small_line;
    }
}

void add_coach_mark(DisplayList& list, const Rect& rect, const CoachMarkLook& look) {
    Item item;
    item.role = Role::coach_mark;
    item.rect = rect;
    item.text = look.title;
    item.look = look;
    list.items.push_back(std::move(item));
}

void draw_spotlight(const Canvas& canvas, const Rect& layer, const SpotlightLook& look) {
    if (!can_draw(canvas) || layer.width <= 0 || layer.height <= 0)
        return;
    const Rect hole = look.cutout;
    const bool hole_open = hole.width > 0 && hole.height > 0 && shares_area(hole, layer);
    if (!hole_open) {
        blend_rect(canvas, layer, colour::backdrop, spotlight_opacity);
        return;
    }
    const int32_t top = hole.y - layer.y;
    if (top > 0)
        blend_rect(
            canvas, {layer.x, layer.y, layer.width, top}, colour::backdrop, spotlight_opacity
        );
    const int32_t bottom = layer.y + layer.height - (hole.y + hole.height);
    if (bottom > 0) {
        blend_rect(
            canvas,
            {layer.x, hole.y + hole.height, layer.width, bottom},
            colour::backdrop,
            spotlight_opacity
        );
    }
    const int32_t left = hole.x - layer.x;
    const int32_t side_h = hole.height;
    if (left > 0 && side_h > 0)
        blend_rect(canvas, {layer.x, hole.y, left, side_h}, colour::backdrop, spotlight_opacity);
    const int32_t right = layer.x + layer.width - (hole.x + hole.width);
    if (right > 0 && side_h > 0) {
        blend_rect(
            canvas,
            {hole.x + hole.width, hole.y, right, side_h},
            colour::backdrop,
            spotlight_opacity
        );
    }
    outline_rect(canvas, hole, colour::accent);
}

void add_spotlight(DisplayList& list, const Rect& layer, const SpotlightLook& look) {
    Item item;
    item.role = Role::spotlight;
    item.rect = layer;
    item.look = look;
    list.items.push_back(std::move(item));
}

PictureWellLayout picture_well_layout(const Rect& rect, bool caption) noexcept {
    PictureWellLayout laid;
    const int32_t caption_h = caption ? compact_metrics.small_line : 0;
    const int32_t gap = caption ? 4 : 0;
    const int32_t border_h = std::max(rect.height - caption_h - gap, int32_t{0});
    laid.border = {rect.x, rect.y, std::max(rect.width, int32_t{0}), border_h};
    const int32_t inner_x = rect.x + 1 + picture_well_inset;
    const int32_t inner_y = rect.y + 1 + picture_well_inset;
    const int32_t inner_w = rect.width - 2 - 2 * picture_well_inset;
    const int32_t inner_h = border_h - 2 - 2 * picture_well_inset;
    if (inner_w > 0 && inner_h > 0)
        laid.inset = {inner_x, inner_y, inner_w, inner_h};
    if (caption && caption_h > 0)
        laid.caption = {
            rect.x, rect.y + border_h + gap, std::max(rect.width, int32_t{0}), caption_h
        };
    return laid;
}

void draw_picture_well(const Canvas& canvas, const Rect& rect, const PictureWellLook& look) {
    if (!can_draw(canvas) || rect.width <= 0 || rect.height <= 0)
        return;
    const PictureWellLayout laid = picture_well_layout(rect, !look.caption.empty());
    if (laid.border.width > 0 && laid.border.height > 0) {
        fill_rect(canvas, laid.border, colour::panel);
        outline_rect(canvas, laid.border, colour::control_border);
    }
    if (look.stand_in && laid.inset.width > 0 && laid.inset.height > 0) {
        fill_rect(canvas, laid.inset, colour::list);
        const int32_t stripe = std::max(laid.inset.width / 3, int32_t{1});
        fill_rect(canvas, {laid.inset.x, laid.inset.y, stripe, laid.inset.height}, colour::accent);
        const int32_t block = std::max(laid.inset.width / 4, int32_t{1});
        const int32_t block_h = std::max(laid.inset.height / 2, int32_t{1});
        fill_rect(
            canvas,
            {laid.inset.x + laid.inset.width - block - 2,
             laid.inset.y + 2,
             block,
             std::min(block_h, laid.inset.height)},
            colour::well
        );
    }
    if (laid.caption.width > 0 && laid.caption.height > 0) {
        box_text(canvas, FontRole::small, look.caption, laid.caption, Align::left, colour::hint, 0);
    }
}

void add_picture_well(DisplayList& list, const Rect& rect, const PictureWellLook& look) {
    Item item;
    item.role = Role::picture_well;
    item.rect = rect;
    item.text = look.caption;
    item.look = look;
    list.items.push_back(std::move(item));
}

void draw_call_to_action(const Canvas& canvas, const Rect& rect, const CallToActionLook& look) {
    if (!can_draw(canvas) || rect.width <= 0 || rect.height <= 0)
        return;
    if (!look.enabled) {
        draw_button(
            canvas, rect, ButtonLook{look.caption, ButtonStyle::plain, false, false, false}
        );
        return;
    }
    ButtonLook button;
    button.caption = look.caption;
    button.style = ButtonStyle::accent;
    button.hovered = look.hovered;
    button.held = look.held;
    button.enabled = true;
    button.bright = !look.held;
    draw_button(canvas, rect, button);

    const uint32_t phase = look.time_ms % call_to_action_period_ms;
    int32_t outset = 1 + static_cast<int32_t>((static_cast<uint64_t>(phase) * 5u) / 1799u);
    int32_t share = 200 - static_cast<int32_t>((static_cast<uint64_t>(phase) * 200u) / 1799u);
    if (look.reduced_motion) {
        outset = 2;
        share = 160;
    }
    if (share < 0)
        share = 0;
    const Rect ring{
        rect.x - outset, rect.y - outset, rect.width + 2 * outset, rect.height + 2 * outset
    };
    blend_frame(canvas, ring, colour::accent, static_cast<uint32_t>(share));
}

void add_call_to_action(
    DisplayList& list,
    const Rect& rect,
    const CallToActionLook& look,
    ControlId id,
    std::string name
) {
    Item item;
    item.role = Role::call_to_action;
    item.rect = rect;
    item.control = id;
    item.text = look.caption;
    item.state.hovered = look.hovered;
    item.state.pressed = look.held;
    item.state.disabled = !look.enabled;
    item.look = look;
    list.items.push_back(std::move(item));
    add_press(list, rect, id, std::move(name), false, false, look.caption, look.enabled);
}

} // namespace oa::ui::kit
