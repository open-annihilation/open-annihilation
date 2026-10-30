// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// List rows, chips, text and search fields, tabs, cards and their grid,
// hover cards and links, drawn through the canvas's primitives. Every colour
// is a kit token and every size a member of compact_metrics.

#include "oa/ui/kit/controls.hpp"

#include "oa/ui/kit/canvas.hpp"
#include "oa/ui/kit/chrome.hpp"
#include "oa/ui/kit/components.hpp"

#include "oa/ui/frontend_renderer/artless.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::ui::kit {

namespace renderer = oa::ui::frontend_renderer;

namespace {

/// The sizes every control here is drawn at.
constexpr const Metrics& kMetrics = compact_metrics;

/// What a cut text ends with.
constexpr std::string_view kEllipsis = "...";
/// The points a list row with the room keeps between its last line and its
/// tags' chips.
constexpr int32_t kChipLead = 2;

/// Tells whether a canvas has a surface to draw on.
///
/// @param canvas the canvas
/// @return true when it has one
bool drawable(const Canvas& canvas) noexcept {
    return can_draw(canvas);
}

/// Returns a text's width on a canvas, 0 when it has no fonts.
///
/// @param canvas the canvas
/// @param role the font
/// @param text the text, in UTF-8
/// @return the width, in points
int32_t measured(const Canvas& canvas, FontRole role, std::string_view text) {
    return measure_text(canvas, role, text);
}

/// Fills a rectangle with a token.
///
/// @param canvas where it is drawn
/// @param rect the rectangle
/// @param ink the colour
void fill(const Canvas& canvas, const Rect& rect, Colour ink) {
    fill_rect(canvas, rect, ink);
}

/// Outlines a rectangle in a token.
///
/// @param canvas where it is drawn
/// @param rect the rectangle
/// @param ink the colour
void outline(const Canvas& canvas, const Rect& rect, Colour ink) {
    outline_rect(canvas, rect, ink);
}

/// Draws a text in a box, when the canvas has a surface and fonts.
///
/// @param canvas where it is drawn
/// @param role the font
/// @param text the text, in UTF-8, as it is drawn
/// @param box the box
/// @param align where it sits along the box
/// @param ink the colour
/// @param tracking extra columns after each glyph but the last
void line(
    const Canvas& canvas,
    FontRole role,
    std::string_view text,
    const Rect& box,
    Align align,
    Colour ink,
    int32_t tracking = 0
) {
    if (!drawable(canvas) || canvas.fonts == nullptr || text.empty())
        return;
    box_text(canvas, role, text, box, align, ink, tracking);
}

/// Draws a text cut with "..." to its box's width.
///
/// @param canvas where it is drawn
/// @param role the font
/// @param text the text, in UTF-8
/// @param box the box; the text is cut to its width
/// @param align where it sits along the box
/// @param ink the colour
void cut_line(
    const Canvas& canvas,
    FontRole role,
    std::string_view text,
    const Rect& box,
    Align align,
    Colour ink
) {
    if (!drawable(canvas) || canvas.fonts == nullptr || text.empty() || box.width <= 0)
        return;
    line(canvas, role, cut_to_width(*canvas.fonts, role, text, box.width), box, align, ink);
}

/// Draws a pill: its face, when it has one, inside a one-point border, or
/// the border alone. The border is the whole pill and the face the pill
/// one point in, so a partial pixel is one colour or the other.
///
/// @param canvas where it is drawn
/// @param rect the pill
/// @param face the face; none leaves what is under the pill
/// @param border the border's colour
void draw_capsule(
    const Canvas& canvas, const Rect& rect, std::optional<Colour> face, Colour border
) {
    if (rect.width <= 0 || rect.height <= 0)
        return;
    const int32_t radius = std::min(rect.width, rect.height) / 2;
    if (!face) {
        outline_round_rect(canvas, rect, radius, border);
        return;
    }
    if (*face == border || rect.width <= 2 || rect.height <= 2) {
        fill_round_rect(canvas, rect, radius, *face);
        return;
    }
    fill_round_rect(canvas, rect, radius, border);
    fill_round_rect(
        canvas,
        {rect.x + 1, rect.y + 1, rect.width - 2, rect.height - 2},
        std::max(radius - 1, int32_t{0}),
        *face
    );
}

/// Draws a rectangle with rounded corners: its face inside a one-point
/// border, the border the whole shape and the face the shape one point in.
///
/// @param canvas where it is drawn
/// @param rect the rectangle
/// @param radius the corners' radius, in points
/// @param face the face
/// @param border the border's colour
void draw_capsule_corners(
    const Canvas& canvas, const Rect& rect, int32_t radius, Colour face, Colour border
) {
    if (rect.width <= 2 || rect.height <= 2) {
        fill_corner_rect(canvas, rect, radius, face);
        return;
    }
    fill_corner_rect(canvas, rect, radius, border);
    fill_corner_rect(
        canvas,
        {rect.x + 1, rect.y + 1, rect.width - 2, rect.height - 2},
        std::max(radius - 1, int32_t{0}),
        face
    );
}

/// Appends a control.
///
/// @param[in,out] list the display list
/// @param rect where it lies
/// @param id its number
/// @param name its automation name
/// @param kind what it is
/// @param steps Left and Right act on it
/// @param checked a chosen one, for automation
/// @param text its caption or value, for automation
/// @param enabled a press reaches it
void add_control(
    DisplayList& list,
    const Rect& rect,
    ControlId id,
    std::string name,
    ControlKind kind,
    bool steps,
    bool checked,
    std::string text,
    bool enabled
) {
    Control control;
    control.id = id;
    control.rect = rect;
    control.enabled = enabled;
    control.name = std::move(name);
    control.kind = kind;
    control.steps = steps;
    control.checked = checked;
    control.text = std::move(text);
    list.controls.push_back(std::move(control));
}

// ---- Chips ----

/// A chip's face, when it has one, its border and its text.
struct ChipColours {
    std::optional<Colour> face{}; ///< the face; none leaves what is under the chip
    Colour border{};              ///< the outline
    Colour text{};                ///< the words
};

/// Returns a chip's colours: a state's tinted fill and its colour's words,
/// the accent filled while playing, and a filter a label's flat tint inside
/// the strong edge, or the selected surface with the accent's edge while it
/// is on.
///
/// @param look the chip
/// @return its face, border and text
ChipColours chip_colours(const ChipLook& look) noexcept {
    if (look.disabled)
        return {std::nullopt, colour::control_border, colour::switch_idle};
    ChipColours colours;
    switch (look.state) {
    case ChipState::get:
        colours = {std::nullopt, colour::control_hover, colour::text_secondary};
        break;
    case ChipState::installed:
        colours = {colour::ok_tint, colour::ok_tint, colour::accent_light};
        break;
    case ChipState::update:
        colours = {colour::caution_tint, colour::caution_tint, colour::lock};
        break;
    case ChipState::playing:
        colours = {colour::accent, colour::accent, colour::on_accent};
        break;
    case ChipState::online:
        colours = {colour::information_tint, colour::information_tint, colour::information};
        break;
    case ChipState::problem:
        colours = {colour::danger_tint, colour::danger_tint, colour::danger};
        break;
    case ChipState::filter:
        if (look.selected)
            colours = {colour::list_selected, colour::accent, colour::text};
        else
            colours = {colour::chip_face, colour::chip_border, colour::text_secondary};
        break;
    case ChipState::tag:
        colours = {std::nullopt, colour::chip_border, colour::hint};
        break;
    }
    if (look.hovered && !colours.face.has_value())
        colours.face = colour::hover;
    else if (look.hovered && look.state != ChipState::playing)
        colours.border = colour::control_hover;
    return colours;
}

/// Returns the colour a chip's state icon is drawn in: while disabled the
/// hovered border's grey, dimmer than every state's, so that a dimmed GET
/// stands apart from the hint of one that can act; else the hint for get,
/// the accent for installed and playing, the lock colour for update, the
/// online colour, the danger colour, and for a filter the accent while it
/// is on and the hint while it is off.
///
/// @param look the chip
/// @return the colour
Colour icon_ink(const ChipLook& look) noexcept {
    if (look.disabled)
        return colour::control_hover;
    switch (look.state) {
    case ChipState::get:
        return colour::hint;
    case ChipState::installed:
    case ChipState::playing:
        return colour::accent;
    case ChipState::update:
        return colour::lock;
    case ChipState::online:
        return colour::online;
    case ChipState::problem:
        return colour::danger;
    case ChipState::filter:
        return look.selected ? colour::accent : colour::hint;
    case ChipState::tag:
        return colour::hint;
    }
    return colour::hint;
}

/// Returns a chip's width on a canvas: its state icon's side, or its text,
/// measured when the canvas has fonts, and chip_padding either side.
///
/// @param canvas the canvas
/// @param look the chip
/// @return the width, in points
int32_t chip_width_on(const Canvas& canvas, const ChipLook& look) {
    if (look.icon.has_value())
        return kMetrics.state_icon_side;
    return measured(canvas, FontRole::small, look.text) + 2 * kMetrics.chip_padding;
}

/// Returns a chip's height: its state icon's side, or chip_height.
///
/// @param look the chip
/// @return the height, in points
int32_t chip_height_of(const ChipLook& look) noexcept {
    return look.icon.has_value() ? kMetrics.state_icon_side : kMetrics.chip_height;
}

/// Returns where a row's chip lies: at the right inset, centred down the row.
///
/// @param row the row
/// @param width the chip's width
/// @param height the chip's height
/// @return the chip's rectangle
Rect chip_in_row(const Rect& row, int32_t width, int32_t height) noexcept {
    return {
        row.x + row.width - kMetrics.list_row_inset - width,
        row.y + (row.height - height) / 2,
        width,
        height,
    };
}

/// Tells whether a mark is drawn under the row's state icon.
///
/// The stack is two icons with no gap, so the row has to be at least that tall.
///
/// @param row the row
/// @param look the row
/// @return true when both are drawn, one under the other
bool maker_under_icon(const Rect& row, const ListRowLook& look) noexcept {
    return look.maker.has_value() && look.chip.has_value() && look.chip->icon.has_value() &&
           row.height >= 2 * kMetrics.state_icon_side;
}

/// Returns the upper square of a centred stack of two state icons.
///
/// @param row the row, at least two icons tall
/// @return the upper square, at the row's right inset
Rect stacked_icon_rect(const Rect& row) noexcept {
    const int32_t side = kMetrics.state_icon_side;
    const int32_t offset = (row.height - 2 * side) / 2;
    return {
        row.x + row.width - kMetrics.list_row_inset - side,
        row.y + offset,
        side,
        side,
    };
}

/// Returns the state icon a maker's mark draws.
///
/// @param mark the mark
/// @return the badge or the bone
StateIcon maker_icon(MakerMark mark) noexcept {
    return mark == MakerMark::open_annihilation ? StateIcon::oa : StateIcon::bone;
}

/// Returns the width a line of text takes in a box both forms of a canvas
/// draw it whole in: the wider of the canvas's measure and the game's
/// fonts', which lay the screens out and cut what does not fit.
///
/// @param canvas the canvas
/// @param role the font
/// @param text the text, in UTF-8
/// @return the width, in points
int32_t whole_width(const Canvas& canvas, FontRole role, std::string_view text) {
    const int32_t game = canvas.fonts != nullptr ? text_width(*canvas.fonts, role, text) : 0;
    return std::max(measured(canvas, role, text), game);
}

/// How tall a run of chips is when a chip that does not fit the line wraps.
struct ChipBlock {
    int32_t height{}; ///< the rows together, including the gaps between them
};

/// Returns the height of chips that wrap within a width.
///
/// A chip wider than the width is left out, as the line stops rather than
/// drawing it past the column. No chips take no height.
///
/// @param canvas where they are measured
/// @param chips the chips
/// @param room the columns one line may use
/// @return the block
ChipBlock chip_block_of(const Canvas& canvas, const std::vector<ChipLook>& chips, int32_t room) {
    ChipBlock block;
    if (chips.empty() || room <= 0)
        return block;
    int32_t left = 0;
    int32_t row_top = 0;
    int32_t row_height = 0;
    for (const ChipLook& chip : chips) {
        const int32_t width = chip_width_on(canvas, chip);
        const int32_t height = chip_height_of(chip);
        if (left > 0 && left + width > room) {
            row_top += row_height + kMetrics.chip_gap;
            row_height = 0;
            left = 0;
        }
        if (left + width > room)
            break;
        row_height = std::max(row_height, height);
        left += width + kMetrics.chip_gap;
        block.height = row_top + row_height;
    }
    return block;
}

/// Draws chips left to right from a point, chip_gap apart. A chip that would
/// pass the column wraps under the line when the row's bottom has room for
/// it, and is left out when it does not. A negative bottom draws one line.
///
/// @param canvas where they are drawn
/// @param chips the chips
/// @param at the first chip's top left
/// @param right the column no chip may pass
/// @param bottom the row under which no chip may extend; negative for one line
void draw_chip_line(
    const Canvas& canvas,
    const std::vector<ChipLook>& chips,
    Point at,
    int32_t right,
    int32_t bottom = -1
) {
    int32_t left = at.x;
    int32_t y = at.y;
    const int32_t home = at.x;
    for (const ChipLook& chip : chips) {
        const int32_t width = chip_width_on(canvas, chip);
        const int32_t height = chip_height_of(chip);
        if (left > home && left + width > right) {
            if (bottom < 0)
                break;
            const int32_t next = y + height + kMetrics.chip_gap;
            if (next + height > bottom)
                break;
            y = next;
            left = home;
        }
        if (left + width > right || (bottom >= 0 && y + height > bottom))
            break;
        draw_chip(canvas, {left, y, width, height}, chip);
        left += width + kMetrics.chip_gap;
    }
}

// ---- List rows ----

/// Draws a list row's badge: the picture, the letters on their colour, or a
/// blank dashed square.
///
/// @param canvas where it is drawn
/// @param badge the badge's square
/// @param look the row
void draw_row_badge(const Canvas& canvas, const Rect& badge, const ListRowLook& look) {
    BadgeLook drawn;
    drawn.picture = look.badge;
    drawn.tag = look.badge_tag;
    drawn.letters = look.badge_text;
    drawn.face = look.badge_colour;
    draw_badge(canvas, badge, drawn);
}

// ---- Text and search fields ----

/// Returns the width of a field's text before its caret, in the regular
/// font. A caret past the text counts as at its end, and one inside a
/// character as at that character's start.
///
/// @param fonts the fonts
/// @param field the text and its caret
/// @return the width, in points
int32_t before_caret(const Fonts& fonts, const TextField& field) {
    const std::string_view text = field.text;
    std::size_t caret = std::min(field.caret, text.size());
    while (caret > 0 && caret < text.size() && continuation_byte(text[caret]))
        --caret;
    return text_width(fonts, FontRole::regular, text.substr(0, caret));
}

// ---- Tabs ----

/// Returns a tab's count, 0 when the look gives none.
///
/// @param look the tabs
/// @param index the tab
/// @return the count
int32_t count_of(const TabsLook& look, std::size_t index) noexcept {
    return index < look.counts.size() ? look.counts[index] : 0;
}

/// Returns the width of a tab's count: its digits and tab_count_padding
/// either side, and never narrower than it is high.
///
/// @param fonts the fonts, or null to count the digits as no columns
/// @param count the count
/// @return the width, in points
int32_t count_width(const Fonts* fonts, int32_t count) {
    const int32_t digits =
        fonts == nullptr ? 0 : text_width(*fonts, FontRole::small, std::to_string(count));
    return std::max(kMetrics.tab_count_height, digits + 2 * kMetrics.tab_count_padding);
}

/// Returns the points either side of a caption: the look's, or the theme's.
///
/// @param look the tabs
/// @return the padding, in points
int32_t tab_side_padding(const TabsLook& look) noexcept {
    return look.side_padding >= 0 ? look.side_padding : kMetrics.tab_padding;
}

/// Returns the points between a caption's characters: the look's, or the theme's.
///
/// @param look the tabs
/// @return the tracking, in points
int32_t tab_letter_gap(const TabsLook& look) noexcept {
    return look.tracking >= 0 ? look.tracking : kMetrics.tab_tracking;
}

/// Returns a tab caption's width in the small font with the given tracking.
///
/// @param fonts the fonts, or null for no columns
/// @param caption the caption
/// @param tracking the points between characters
/// @return the width, in points
int32_t caption_width(const Fonts* fonts, std::string_view caption, int32_t tracking) {
    if (fonts == nullptr || caption.empty())
        return 0;
    return tracked_width(*fonts, FontRole::small, caption, tracking);
}

// ---- Cards ----

/// Returns a card's preview: the largest square that leaves room for the
/// card's two lines and three insets, centred across the card, card_inset
/// under its top.
///
/// @param rect the card
/// @return the preview's square
Rect card_preview(const Rect& rect) noexcept {
    const int32_t lines = kMetrics.regular_line + kMetrics.small_line + 3 * kMetrics.card_inset;
    const int32_t side =
        std::max(std::min(rect.width - 2 * kMetrics.card_inset, rect.height - lines), int32_t{0});
    return {rect.x + (rect.width - side) / 2, rect.y + kMetrics.card_inset, side, side};
}

// ---- Hover cards ----

/// Tells whether a hover card has a header line.
///
/// @param look the card
/// @return true when it shows the mark, a title or an aside
bool has_header(const HoverCardLook& look) noexcept {
    return look.mark || !look.title.empty() || !look.aside.empty();
}

/// Returns the column a hover card's values start at, from its text's left:
/// the widest label and hover_card_padding, or 0 when no row has a label.
///
/// @param fonts the fonts, or null to measure nothing
/// @param look the card
/// @return the column, in points
int32_t label_column(const Fonts* fonts, const HoverCardLook& look) {
    bool labelled = false;
    int32_t widest = 0;
    for (const HoverCardRow& row : look.rows) {
        if (row.label.empty())
            continue;
        labelled = true;
        if (fonts != nullptr)
            widest = std::max(widest, text_width(*fonts, FontRole::small, row.label));
    }
    return labelled ? widest + kMetrics.hover_card_padding : 0;
}

/// Draws a hover card's arrow: a triangle reaching hover_card_arrow out of
/// one edge, its point arrow_at along that edge, drawn as nested bars.
///
/// @param canvas where it is drawn
/// @param rect the card
/// @param look the card's arrow
void draw_arrow(const Canvas& canvas, const Rect& rect, const HoverCardLook& look) {
    const int32_t depth = kMetrics.hover_card_arrow;
    for (int32_t half = 0; half < depth; ++half) {
        const int32_t reach = depth - half;
        const int32_t across = 2 * half + 1;
        Rect bar{};
        switch (look.arrow) {
        case Side::none:
            return;
        case Side::top:
            bar = {rect.x + look.arrow_at - half, rect.y - reach, across, reach};
            break;
        case Side::bottom:
            bar = {rect.x + look.arrow_at - half, rect.y + rect.height, across, reach};
            break;
        case Side::left:
            bar = {rect.x - reach, rect.y + look.arrow_at - half, reach, across};
            break;
        case Side::right:
            bar = {rect.x + rect.width, rect.y + look.arrow_at - half, reach, across};
            break;
        }
        fill(canvas, bar, colour::strong_edge);
    }
}

/// Tells whether a byte is white space a shown name sheds at either end.
///
/// ASCII only, so a name trims the same way on every machine.
///
/// @param byte the byte
/// @return true for space, tab or a line break
bool ascii_space(unsigned char byte) noexcept {
    return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r' || byte == '\f' ||
           byte == '\v';
}

/// Returns a text with white space taken off both ends.
///
/// @param text the text
/// @return the text inside that white space; empty when it was only white space
std::string_view trimmed_ends(std::string_view text) noexcept {
    while (!text.empty() && ascii_space(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    while (!text.empty() && ascii_space(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);
    return text;
}

} // namespace

std::string cut_to_width(const Fonts& fonts, FontRole role, std::string_view text, int32_t width) {
    if (text_width(fonts, role, text) <= width)
        return std::string(text);
    std::string best(kEllipsis);
    for (std::size_t end = 0; end < text.size();) {
        end += character_bytes(text.substr(end));
        std::string shown(text.substr(0, end));
        shown += kEllipsis;
        if (text_width(fonts, role, shown) > width)
            break;
        best = std::move(shown);
    }
    return best;
}

// ---- List rows ----

namespace {

/// Returns a tag's width in a row with a spacing: its state icon's side, or
/// its words in the screen's fonts, the measure the screen laid the row out
/// in, so the tags wrap where it counted them, and the spacing's padding
/// either side. A drawn face a little wider stays inside that padding.
///
/// @param fonts the fonts; null measures in the canvas's form
/// @param canvas where it is measured when there are no fonts
/// @param chip the tag
/// @param spacing the row's spacing
/// @return the width, in points
int32_t spaced_tag_width(
    const Fonts* fonts, const Canvas* canvas, const ChipLook& chip, const RowSpacing& spacing
) {
    if (chip.icon.has_value())
        return kMetrics.state_icon_side;
    const int32_t words = fonts != nullptr    ? text_width(*fonts, FontRole::small, chip.text)
                          : canvas != nullptr ? measured(*canvas, FontRole::small, chip.text)
                                              : 0;
    return words + 2 * spacing.tag_padding;
}

/// Returns a tag's width on a canvas in a row with a spacing.
///
/// @param canvas where it is drawn
/// @param chip the tag
/// @param spacing the row's spacing
/// @return the width, in points
int32_t spaced_tag_width(const Canvas& canvas, const ChipLook& chip, const RowSpacing& spacing) {
    return spaced_tag_width(canvas.fonts, &canvas, chip, spacing);
}

} // namespace

Rect list_row_chip_rect(const Fonts& fonts, const Rect& row, const ChipLook& chip) {
    return chip_in_row(row, chip_width(fonts, chip), chip_height_of(chip));
}

Rect list_row_icon_rect(const Fonts& fonts, const Rect& row, const ListRowLook& look) {
    if (!look.chip.has_value())
        return {};
    if (maker_under_icon(row, look))
        return stacked_icon_rect(row);
    if (look.spacing.has_value() && !look.chip->icon.has_value())
        return chip_in_row(
            row,
            spaced_tag_width(&fonts, nullptr, *look.chip, *look.spacing),
            look.spacing->tag_height
        );
    return list_row_chip_rect(fonts, row, *look.chip);
}

Rect list_row_maker_rect(const Rect& row, const ListRowLook& look) {
    if (!look.maker.has_value())
        return {};
    const int32_t side = kMetrics.state_icon_side;
    if (look.chip.has_value() && look.chip->icon.has_value()) {
        if (!maker_under_icon(row, look))
            return {};
        const Rect icon = stacked_icon_rect(row);
        return {icon.x, icon.y + side, side, side};
    }
    return {
        row.x + row.width - kMetrics.list_row_inset - side,
        row.y + (row.height - side) / 2,
        side,
        side,
    };
}

namespace {

/// Walks a row's tags as a spacing lays them: left to right, tag_gap apart,
/// a tag that would pass the column starting a line tag_gap under the one
/// before. A tag wider than the line ends the walk. Each placed tag is
/// handed to place.
///
/// @param canvas where they are measured
/// @param tags the tags
/// @param at the first tag's top left
/// @param right the column no tag may pass
/// @param spacing the row's spacing
/// @param place called with each tag and its rectangle
/// @return the rows the tags take, gaps included; 0 for none
template <typename Place>
int32_t walk_spaced_tags(
    const Canvas& canvas,
    const std::vector<ChipLook>& tags,
    Point at,
    int32_t right,
    const RowSpacing& spacing,
    Place&& place
) {
    int32_t left = at.x;
    int32_t y = at.y;
    int32_t bottom = at.y;
    for (const ChipLook& chip : tags) {
        const int32_t width = spaced_tag_width(canvas, chip, spacing);
        if (left > at.x && left + width > right) {
            left = at.x;
            y = bottom + spacing.tag_gap;
        }
        if (left + width > right)
            break;
        const int32_t height =
            chip.icon.has_value() ? kMetrics.state_icon_side : spacing.tag_height;
        place(chip, Rect{left, y, width, height});
        bottom = std::max(bottom, y + height);
        left += width + spacing.tag_gap;
    }
    return bottom - at.y;
}

/// Draws a list row's title, and its byline after it, on the title's line.
///
/// @param canvas where it is drawn
/// @param look the row
/// @param text_left the title's column
/// @param text_right the column the title and byline end before
/// @param title_row the line's top
void draw_row_title(
    const Canvas& canvas,
    const ListRowLook& look,
    int32_t text_left,
    int32_t text_right,
    int32_t title_row
) {
    const int32_t room = std::max(text_right - text_left, int32_t{0});
    const Rect title_box{text_left, title_row, room, kMetrics.regular_line};
    // Trim only what the row shows. The maker is placed from the width the
    // title is drawn at (bold on the smooth form), plus list_row_gap, so a
    // wider drawn face cannot eat the space before "by".
    const std::string_view trimmed = trimmed_ends(look.title);
    if (canvas.fonts != nullptr && !trimmed.empty()) {
        const std::string title = cut_to_width(*canvas.fonts, FontRole::regular, trimmed, room);
        const int32_t drawn = measure_text(canvas, FontRole::regular, title);
        const int32_t title_width = std::min(drawn, room);
        const Rect fitted{text_left, title_row, title_width, kMetrics.regular_line};
        line(canvas, FontRole::regular, title, fitted, Align::left, colour::text);
        const int32_t by_left = text_left + title_width + kMetrics.list_row_gap;
        // The byline shows where at least "..." fits after the title; in a
        // narrower gap it is left out rather than drawn past its box.
        if (!look.by.empty() &&
            text_right - by_left >= measure_text(canvas, FontRole::small, kEllipsis)) {
            const Rect by_box{by_left, title_row, text_right - by_left, kMetrics.regular_line};
            cut_line(
                canvas,
                FontRole::small,
                look.by,
                by_box,
                Align::left,
                look.by_colour.value_or(colour::hint)
            );
        }
    } else if (canvas.fonts != nullptr && !look.by.empty()) {
        cut_line(
            canvas,
            FontRole::small,
            look.by,
            title_box,
            Align::left,
            look.by_colour.value_or(colour::hint)
        );
    }
}

/// Draws the words and tags of a row that has a spacing: the title's line,
/// the lines under it title_gap down and line_step apart, and the tags
/// tags_gap under the last line, as one block centred down the row. The
/// aside's lines stand line_step apart at the right, the first centred on
/// the title's line.
///
/// @param canvas where it is drawn
/// @param rect the row
/// @param look the row's look, whose spacing is set
/// @param text_left the title's column
/// @param right the column the words end before: left of the chip or the mark
void draw_spaced_row_text(
    const Canvas& canvas,
    const Rect& rect,
    const ListRowLook& look,
    int32_t text_left,
    int32_t right
) {
    const RowSpacing& spacing = *look.spacing;
    int32_t aside_width = 0;
    if (!look.aside.empty()) {
        for (const std::string& entry : look.aside)
            aside_width = std::max(aside_width, whole_width(canvas, FontRole::small, entry));
        aside_width = std::min(aside_width, std::max(right - text_left, int32_t{0}));
    }
    const int32_t text_right =
        look.aside.empty() ? right : right - aside_width - kMetrics.list_row_gap;
    const auto line_count = static_cast<int32_t>(look.lines.size());
    const int32_t lines_height =
        line_count > 0 ? (line_count - 1) * spacing.line_step + kMetrics.small_line : 0;
    const int32_t tags_height = walk_spaced_tags(
        canvas, look.tags, {text_left, 0}, text_right, spacing, [](const ChipLook&, const Rect&) {}
    );
    int32_t block = kMetrics.regular_line;
    if (line_count > 0)
        block += spacing.title_gap + lines_height;
    if (tags_height > 0)
        block += (line_count > 0 ? spacing.tags_gap : spacing.title_gap) + tags_height;
    const int32_t title_row = rect.y + std::max((rect.height - block) / 2, int32_t{0});

    if (!look.aside.empty()) {
        // As wide as its widest entry in either measure, so that no entry,
        // a version such as "1 · rev 3" among them, is cut on either form.
        const int32_t aside_left = right - aside_width;
        const int32_t aside_row = title_row + (kMetrics.regular_line - kMetrics.small_line) / 2;
        for (std::size_t index = 0; index < look.aside.size(); ++index) {
            const Rect box{
                aside_left,
                aside_row + static_cast<int32_t>(index) * spacing.line_step,
                aside_width,
                kMetrics.small_line,
            };
            cut_line(
                canvas,
                FontRole::small,
                look.aside[index],
                box,
                Align::right,
                colour::text_secondary
            );
        }
    }

    draw_row_title(canvas, look, text_left, text_right, title_row);
    const int32_t room = std::max(text_right - text_left, int32_t{0});
    int32_t line_row = title_row + kMetrics.regular_line + spacing.title_gap;
    for (const std::string& text : look.lines) {
        cut_line(
            canvas,
            FontRole::small,
            text,
            {text_left, line_row, room, kMetrics.small_line},
            Align::left,
            colour::list_text
        );
        line_row += spacing.line_step;
    }
    const int32_t tags_top =
        line_count > 0 ? line_row - spacing.line_step + kMetrics.small_line + spacing.tags_gap
                       : line_row;
    const int32_t bottom = rect.y + rect.height;
    static_cast<void>(walk_spaced_tags(
        canvas,
        look.tags,
        {text_left, tags_top},
        text_right,
        spacing,
        [&](const ChipLook& chip, const Rect& place) {
            if (place.y + place.height <= bottom)
                draw_chip(canvas, place, chip);
        }
    ));
}

} // namespace

void draw_list_row(const Canvas& canvas, const Rect& rect, const ListRowLook& look) {
    if (!drawable(canvas))
        return;
    // Rounded as a control is: the hover face, then a selected row's accent
    // tint over it and the accent's outline. A selection bar is the nav
    // entry's fill and lime bar instead (chrome.cpp's marker, 5 by 2 by 8),
    // and the accent outline stays off. The keyboard focus draws its own ring.
    const int32_t row_radius = kMetrics.control_radius;
    if (look.selected && look.selection_bar) {
        constexpr int32_t marker_offset = 5;
        constexpr int32_t marker_width = 2;
        constexpr int32_t marker_height = 8;
        fill_corner_rect(canvas, rect, row_radius, colour::list_selected);
        fill_rect(
            canvas,
            {rect.x + marker_offset,
             rect.y + (rect.height - marker_height) / 2,
             marker_width,
             marker_height},
            colour::accent
        );
    } else {
        if (look.hovered)
            fill_corner_rect(canvas, rect, row_radius, colour::hover);
        if (look.selected) {
            blend_corner_rect(canvas, rect, row_radius, colour::accent, selected_tint);
            outline_corner_rect(canvas, rect, row_radius, colour::accent);
        }
    }

    const int32_t inset = kMetrics.list_row_inset;
    int32_t text_left = rect.x + inset;
    if (look.show_badge) {
        const int32_t side =
            std::max(std::min(rect.height - 2 * inset, kMetrics.list_row_badge), int32_t{0});
        const Rect badge{rect.x + inset, rect.y + (rect.height - side) / 2, side, side};
        draw_row_badge(canvas, badge, look);
        text_left = badge.x + side + inset;
    } else if (look.lead > 0) {
        text_left = rect.x + inset + look.lead;
    }

    int32_t right = rect.x + rect.width - inset;
    if (look.chip.has_value()) {
        // A row with a spacing sizes a worded chip as its tags.
        const bool spaced_chip = look.spacing.has_value() && !look.chip->icon.has_value();
        const Rect chip =
            maker_under_icon(rect, look) ? stacked_icon_rect(rect)
            : spaced_chip
                ? chip_in_row(
                      rect,
                      spaced_tag_width(canvas, *look.chip, *look.spacing),
                      look.spacing->tag_height
                  )
                : chip_in_row(rect, chip_width_on(canvas, *look.chip), chip_height_of(*look.chip));
        draw_chip(canvas, chip, *look.chip);
        right = chip.x - kMetrics.list_row_gap;
    }
    const Rect mark = list_row_maker_rect(rect, look);
    if (mark.width > 0 && mark.height > 0) {
        draw_state_icon(canvas, mark, maker_icon(*look.maker), colour::text);
        if (!look.chip.has_value())
            right = mark.x - kMetrics.list_row_gap;
    }

    if (look.spacing.has_value()) {
        draw_spaced_row_text(canvas, rect, look, text_left, right);
        return;
    }

    // The text: the title's line, then each line, then the tags' lines,
    // centred down the row. Chips that do not fit one line wrap, and the
    // block includes every line so the wrap stays inside the row. A short
    // row's text fills it from its top. A row with the room keeps
    // list_row_line_gap clear under the title's line and over the tags, so
    // the lines and the chips stand apart. A row with less keeps the tags a
    // little below the line over them when it can.
    int32_t aside_width = 0;
    if (!look.aside.empty()) {
        for (const std::string& entry : look.aside)
            aside_width = std::max(aside_width, whole_width(canvas, FontRole::small, entry));
        aside_width = std::min(aside_width, std::max(right - text_left, int32_t{0}));
    }
    const int32_t text_limit =
        look.aside.empty() ? right : right - aside_width - kMetrics.list_row_gap;
    const int32_t chip_room = std::max(text_limit - text_left, int32_t{0});
    const int32_t chip_span = chip_block_of(canvas, look.tags, chip_room).height;
    const auto line_count = static_cast<int32_t>(look.lines.size()) + (chip_span > 0 ? 1 : 0);
    const int32_t packed_height = std::max(
                                      kMetrics.list_row_title_top + kMetrics.regular_line,
                                      kMetrics.list_row_line_top + line_count * kMetrics.small_line
                                  ) +
                                  kMetrics.list_row_title_top;
    const auto lines_height = static_cast<int32_t>(look.lines.size()) * kMetrics.small_line;
    const int32_t spaced_height =
        kMetrics.list_row_title_top + kMetrics.regular_line + kMetrics.list_row_line_gap +
        lines_height +
        (chip_span <= 0 ? 0 : (look.lines.empty() ? 0 : kMetrics.list_row_line_gap) + chip_span) +
        kMetrics.list_row_title_top;
    const bool spaced = line_count > 0 && rect.height >= spaced_height;
    const int32_t chip_lead = spaced ? (look.lines.empty() ? 0 : kMetrics.list_row_line_gap)
                              : !look.tags.empty() && rect.height >= packed_height + kChipLead
                                  ? kChipLead
                                  : 0;
    const int32_t text_height = spaced ? spaced_height : packed_height + chip_lead;
    const int32_t top = rect.y + std::max((rect.height - text_height) / 2, int32_t{0});
    const int32_t title_row = top + kMetrics.list_row_title_top;
    // The aside's lines are the small font's, the first centred on the title's line.
    const int32_t aside_row = title_row + (kMetrics.regular_line - kMetrics.small_line) / 2;

    int32_t text_right = right;
    if (!look.aside.empty()) {
        // As wide as its widest entry in either measure, so that no entry,
        // a version such as "1 · rev 3" among them, is cut on either form.
        const int32_t aside_left = right - aside_width;
        for (std::size_t index = 0; index < look.aside.size(); ++index) {
            const Rect box{
                aside_left,
                aside_row + static_cast<int32_t>(index) * kMetrics.small_line,
                aside_width,
                kMetrics.small_line,
            };
            cut_line(
                canvas,
                FontRole::small,
                look.aside[index],
                box,
                Align::right,
                colour::text_secondary
            );
        }
        text_right = aside_left - kMetrics.list_row_gap;
    }

    const int32_t room = std::max(text_right - text_left, int32_t{0});
    draw_row_title(canvas, look, text_left, text_right, title_row);

    int32_t line_row = top + (spaced ? kMetrics.list_row_title_top + kMetrics.regular_line +
                                           kMetrics.list_row_line_gap
                                     : kMetrics.list_row_line_top);
    for (const std::string& text : look.lines) {
        cut_line(
            canvas,
            FontRole::small,
            text,
            {text_left, line_row, room, kMetrics.small_line},
            Align::left,
            colour::list_text
        );
        line_row += kMetrics.small_line;
    }
    draw_chip_line(
        canvas, look.tags, {text_left, line_row + chip_lead}, text_right, rect.y + rect.height
    );
}

void add_list_row(
    DisplayList& list, const Rect& rect, const ListRowLook& look, ControlId id, std::string name
) {
    Item item;
    item.role = Role::list_row;
    item.rect = rect;
    item.control = id;
    item.text = look.title;
    item.state.hovered = look.hovered;
    item.state.selected = look.selected;
    item.look = look;
    list.items.push_back(std::move(item));
    add_control(
        list,
        rect,
        id,
        std::move(name),
        ControlKind::list_item,
        false,
        look.selected,
        look.title,
        true
    );
}

// ---- Chips ----

int32_t chip_width(const Fonts& fonts, const ChipLook& look) {
    if (look.icon.has_value())
        return kMetrics.state_icon_side;
    return text_width(fonts, FontRole::small, look.text) + 2 * kMetrics.chip_padding;
}

void draw_chip(const Canvas& canvas, const Rect& rect, const ChipLook& look) {
    if (!drawable(canvas))
        return;
    if (look.icon.has_value()) {
        if (look.hovered && !look.disabled)
            fill_round_rect(canvas, rect, std::min(rect.width, rect.height) / 2, colour::hover);
        draw_state_icon(clipped(canvas, rect), rect, *look.icon, icon_ink(look));
        return;
    }
    const ChipColours colours = chip_colours(look);
    draw_capsule(canvas, rect, colours.face, colours.border);
    line(clipped(canvas, rect), FontRole::small, look.text, rect, Align::centre, colours.text);
}

// ---- Pills ----

namespace {

/// A pill's ground, its text, and its outline when the state draws one.
struct PillColours {
    Colour ground{};                 ///< the rounded ground
    Colour text{};                   ///< the words
    std::optional<Colour> outline{}; ///< the outline; none when the ground is the whole pill
    bool ring{};                     ///< the outline alone: no ground, what is under it shows
};

/// Returns a pill's colours from its state alone: a badge's. Playing and
/// active are the accent filled, with on_accent text. Neutral and inactive
/// are the selected surface under the secondary text. Installed, update,
/// problem, information and brand are their colour's tinted fill under
/// words in that colour. A tag is a tag chip's outline round hint words.
///
/// @param state the state
/// @return its colours
PillColours colours_for(PillState state) noexcept {
    switch (state) {
    case PillState::playing:
    case PillState::active:
        return {colour::accent, colour::on_accent, std::nullopt};
    case PillState::neutral:
    case PillState::inactive:
        return {colour::list_selected, colour::text_secondary, std::nullopt};
    case PillState::installed:
        return {colour::ok_tint, colour::accent_light, std::nullopt};
    case PillState::update:
        return {colour::caution_tint, colour::lock, std::nullopt};
    case PillState::problem:
        return {colour::danger_tint, colour::danger, std::nullopt};
    case PillState::information:
        return {colour::information_tint, colour::information, std::nullopt};
    case PillState::brand:
        return {colour::brand_tint, colour::brand_light, std::nullopt};
    case PillState::tag:
        return {colour::chip_face, colour::hint, colour::chip_border, true};
    }
    return {colour::list_selected, colour::text_secondary, std::nullopt};
}

/// Returns a pill's colours, including hover and disabled.
///
/// Hover lightens an accent ground and outlines any other in the hovered
/// border's grey; a tag's outline turns the hovered secondary border's and
/// its words the secondary text. Disabled is the hover surface under the
/// idle colour and ignores hover. Focus is the focus ring, drawn round the
/// pill.
///
/// @param look the pill
/// @return its colours
PillColours pill_colours(const PillLook& look) noexcept {
    if (look.disabled)
        return {colour::hover, colour::switch_idle, std::nullopt};
    PillColours colours = colours_for(look.state);
    const bool accent_ground = look.state == PillState::playing || look.state == PillState::active;
    if (look.hovered && accent_ground) {
        colours.ground = colour::accent_light;
    } else if (look.hovered && colours.ring) {
        colours.outline = colour::raised_border_hover;
        colours.text = colour::text_secondary;
    } else if (look.hovered) {
        colours.outline = colour::control_hover;
    }
    return colours;
}

/// Returns the padding either side of a pill's word, in points.
///
/// Each end is a semicircle of half the pill's height. A word then keeps
/// chip_padding clear of that curve, 6 points at 1x, and the points scale
/// with the interface. An empty pill has no word, so the padding is only
/// the semicircle and the pill stays a circle.
///
/// @param look the pill
/// @return the padding, in points
int32_t pill_padding(const PillLook& look) noexcept {
    const int32_t curve = kMetrics.chip_height / 2;
    if (look.text.empty())
        return curve;
    return curve + kMetrics.chip_padding;
}

/// The gap between a pill's text and an icon inside it.
constexpr int32_t kPillIconGap = 2;

/// Returns the side of an icon drawn inside a pill: two points short of the
/// pill's height, so the icon sits inside the capsule.
///
/// @return the side, in points
int32_t pill_icon_side() noexcept {
    return std::max(kMetrics.chip_height - 2, int32_t{0});
}

/// Returns the extra width an icon adds to a pill.
///
/// @param look the pill
/// @return the gap and the icon; 0 when the pill has no icon
/// Returns the extra width one icon inside a pill adds.
///
/// @param present true when the pill draws that icon
/// @return the gap and the icon; 0 when it draws none
int32_t pill_one_icon_extra(bool present) noexcept {
    if (!present)
        return 0;
    return kPillIconGap + pill_icon_side();
}

int32_t pill_icon_extra(const PillLook& look) noexcept {
    return pill_one_icon_extra(look.tag.has_value()) + pill_one_icon_extra(look.icon.has_value());
}

} // namespace

int32_t pill_width(const Fonts& fonts, const PillLook& look) {
    return std::max(
        text_width(fonts, FontRole::small, look.text) + 2 * pill_padding(look) +
            pill_icon_extra(look),
        kMetrics.chip_height
    );
}

int32_t pill_width_of(const Canvas& canvas, const PillLook& look) {
    int32_t words = measure_text(canvas, FontRole::small, look.text);
    if (words <= 0 && canvas.fonts != nullptr)
        words = text_width(*canvas.fonts, FontRole::small, look.text);
    if (words < 0)
        words = 0;
    return std::max(words + 2 * pill_padding(look) + pill_icon_extra(look), kMetrics.chip_height);
}

void draw_pill(const Canvas& canvas, const Rect& rect, const PillLook& look) {
    if (!drawable(canvas) || rect.width <= 0 || rect.height <= 0)
        return;
    const int32_t radius = std::min(rect.width, rect.height) / 2;
    const PillColours colours = pill_colours(look);
    if (colours.ring && colours.outline.has_value()) {
        // A tag: the outline alone, the ground under it left as it is.
        outline_round_rect(canvas, rect, radius, *colours.outline);
    } else if (colours.outline.has_value()) {
        // The outline is the whole capsule, and the ground is the capsule
        // one point in, so a partial pixel is one colour or the other.
        fill_round_rect(canvas, rect, radius, *colours.outline);
        if (rect.width > 2 && rect.height > 2) {
            const Rect inner{rect.x + 1, rect.y + 1, rect.width - 2, rect.height - 2};
            fill_round_rect(canvas, inner, std::max(radius - 1, int32_t{0}), colours.ground);
        }
    } else {
        fill_round_rect(canvas, rect, radius, colours.ground);
    }
    Rect words = rect;
    // An icon stands as far in from the pill's end as from its top, so a
    // pill taller than chip_height keeps its icons inside the round ends.
    const int32_t icon_side = std::min(pill_icon_side(), std::max(rect.height - 2, int32_t{0}));
    const int32_t icon_inset = std::max((rect.height - icon_side) / 2, int32_t{1});
    if (look.tag.has_value()) {
        const int32_t side = icon_side;
        const int32_t icon_x = rect.x + icon_inset;
        const Rect icon{icon_x, rect.y + (rect.height - side) / 2, side, side};
        const int32_t words_x = icon_x + side + kPillIconGap;
        words.x = words_x;
        words.width = std::max(rect.x + rect.width - words_x, int32_t{0});
        draw_tag_icon(clipped(canvas, rect), icon, *look.tag, colours.text);
    }
    if (look.icon.has_value()) {
        const int32_t side = icon_side;
        const int32_t icon_x = rect.x + rect.width - icon_inset - side;
        const Rect icon{icon_x, rect.y + (rect.height - side) / 2, side, side};
        words.width = std::max(icon_x - kPillIconGap - words.x, int32_t{0});
        draw_state_icon(clipped(canvas, rect), icon, *look.icon, colours.text);
    }
    // box_text, not line: line draws nothing on the smooth form when the
    // game's fonts are absent, and a pill's words are part of the shape.
    box_text(clipped(canvas, rect), FontRole::small, look.text, words, Align::centre, colours.text);
    if (look.focused && !look.disabled) {
        const int32_t out = kMetrics.focus_inset;
        ring_corner_rect(
            canvas, grown(rect, out), radius + out, kMetrics.focus_ring, colour::focus
        );
    }
}

void add_pill(
    DisplayList& list, const Rect& rect, const PillLook& look, ControlId id, std::string name
) {
    Item item;
    item.role = Role::pill;
    item.rect = rect;
    item.control = id;
    item.text = look.text;
    item.state.hovered = look.hovered;
    item.state.focused = look.focused;
    item.state.selected = look.state == PillState::active;
    item.state.disabled = look.disabled;
    item.look = look;
    list.items.push_back(std::move(item));
    if (id == no_control)
        return;
    add_control(
        list,
        rect,
        id,
        std::move(name),
        ControlKind::button,
        false,
        look.state == PillState::active,
        look.text,
        !look.disabled
    );
}

void add_chip(
    DisplayList& list, const Rect& rect, const ChipLook& look, ControlId id, std::string name
) {
    Item item;
    item.role = Role::chip;
    item.rect = rect;
    item.control = id;
    item.text = look.text;
    item.state.hovered = look.hovered;
    item.state.selected = look.selected;
    item.state.disabled = look.disabled;
    item.look = look;
    list.items.push_back(std::move(item));
    if (id == no_control)
        return;
    add_control(
        list,
        rect,
        id,
        std::move(name),
        ControlKind::button,
        false,
        look.selected,
        look.text,
        !look.disabled
    );
}

// ---- Text and search fields ----

Rect field_text_rect(const Rect& rect, const SearchLook& look) noexcept {
    int32_t left = rect.x + kMetrics.field_inset;
    if (look.magnifier)
        left += kMetrics.magnifier_side + kMetrics.field_inset;
    const int32_t right = rect.x + rect.width - kMetrics.field_inset;
    return {left, rect.y, std::max(right - left, int32_t{0}), rect.height};
}

int32_t field_scroll(const Fonts& fonts, const Rect& rect, const SearchLook& look) {
    const int32_t room = field_text_rect(rect, look).width - kMetrics.caret_width;
    return std::max(before_caret(fonts, look.field) - room, int32_t{0});
}

void draw_search(const Canvas& canvas, const Rect& rect, const SearchLook& look) {
    if (!drawable(canvas))
        return;
    const int32_t radius = control_radius_for(rect.height);
    Colour border = colour::control_border;
    if (look.focused)
        border = colour::accent;
    else if (look.hovered)
        border = colour::control_hover;
    if (look.focused)
        ring_corner_rect(
            canvas,
            grown(rect, kMetrics.focus_inset),
            radius + kMetrics.focus_inset,
            kMetrics.focus_inset,
            colour::field_ring
        );
    draw_capsule_corners(canvas, rect, radius, colour::well, border);
    if (look.magnifier)
        draw_mark(
            canvas,
            Mark::magnifier,
            {rect.x + kMetrics.field_inset, rect.y + (rect.height - kMetrics.magnifier_side) / 2},
            colour::hint
        );

    const Rect box = field_text_rect(rect, look);
    const Canvas inside = clipped(canvas, box);
    if (look.field.text.empty()) {
        cut_line(inside, FontRole::regular, look.placeholder, box, Align::left, colour::hint);
    } else if (canvas.fonts != nullptr) {
        const int32_t scroll = field_scroll(*canvas.fonts, rect, look);
        line(
            inside,
            FontRole::regular,
            look.field.text,
            {box.x - scroll, box.y, box.width + scroll, box.height},
            Align::left,
            colour::text
        );
    }
    if (!look.focused || canvas.fonts == nullptr)
        return;
    const int32_t before = before_caret(*canvas.fonts, look.field);
    const int32_t scroll = field_scroll(*canvas.fonts, rect, look);
    const int32_t rows = std::min(capitals(*canvas.fonts, FontRole::regular), box.height);
    fill(
        inside,
        {box.x + before - scroll, box.y + (box.height - rows) / 2, kMetrics.caret_width, rows},
        colour::text
    );
}

void add_search(
    DisplayList& list, const Rect& rect, const SearchLook& look, ControlId id, std::string name
) {
    Item item;
    item.role = Role::field;
    item.rect = rect;
    item.control = id;
    item.text = look.field.text;
    item.state.hovered = look.hovered;
    item.state.focused = look.focused;
    item.look = look;
    list.items.push_back(std::move(item));
    add_control(
        list, rect, id, std::move(name), ControlKind::text_field, true, false, look.field.text, true
    );
}

// ---- Tabs ----

/// Tells whether a tab draws its count.
///
/// @param look the tabs
/// @param count the tab's count
/// @return true when the count is drawn
bool count_shown(const TabsLook& look, int32_t count) noexcept {
    if (look.count_style == TabCountStyle::words)
        return count >= 0;
    return count > 0;
}

/// Returns the columns a drawn count takes after the caption.
///
/// @param fonts the fonts, or null to count a word-style count as no columns
/// @param look the tabs
/// @param count the count
/// @return the width, in points; 0 when the count is not drawn
int32_t shown_count_width(const Fonts* fonts, const TabsLook& look, int32_t count) {
    if (!count_shown(look, count))
        return 0;
    if (look.count_style == TabCountStyle::words) {
        if (fonts == nullptr)
            return 0;
        return text_width(*fonts, FontRole::small, std::to_string(count));
    }
    return count_width(fonts, count);
}

std::vector<Rect> tab_rects(const Fonts& fonts, const Rect& strip, const TabsLook& look) {
    std::vector<Rect> rects;
    rects.reserve(look.captions.size());
    int32_t left = strip.x;
    const int32_t side = tab_side_padding(look);
    const int32_t letter_gap = tab_letter_gap(look);
    for (std::size_t index = 0; index < look.captions.size(); ++index) {
        int32_t width = 2 * side + caption_width(&fonts, look.captions[index], letter_gap);
        const int32_t count = count_of(look, index);
        const int32_t digits = shown_count_width(&fonts, look, count);
        if (digits > 0)
            width += kMetrics.tab_count_gap + digits;
        rects.push_back({left, strip.y, width, strip.height});
        left += width;
    }
    return rects;
}

namespace {

/// How wide a tab-strip arrow is, in points.
constexpr int32_t kTabArrow = 12;

/// A chevron's side, in points. The right one is Mark::arrow_closed.
constexpr int32_t kChevron = 5;

/// A left chevron, 5 by 5, the right one mirrored.
constexpr std::array<uint8_t, 5 * 5> kTabArrowLeftBits{
    0, 0, 0, 1, 0, //
    0, 0, 1, 1, 0, //
    0, 1, 1, 1, 0, //
    0, 0, 1, 1, 0, //
    0, 0, 0, 1, 0, //
};

/// The left chevron.
constexpr renderer::Mark kTabArrowLeft{kChevron, kChevron, kTabArrowLeftBits};

/// How a scroll sits in a strip: which arrows show, and how much is visible.
struct TabMeasure {
    bool left{};          ///< an arrow shows at the left
    bool right{};         ///< an arrow shows at the right
    int32_t visible{};    ///< the caption area's width, in points
    int32_t max_scroll{}; ///< the furthest scroll, in points
};

/// Measures a strip at one scroll. A row that fits has no arrows. The right
/// arrow is kept only when the tabs still run past the view it leaves.
///
/// @param total the tabs' width, in points
/// @param strip_width the strip's width, in points
/// @param scroll the scroll asked for, in points
/// @param arrow an arrow's width, in points
/// @return the measure
TabMeasure measure_strip(int32_t total, int32_t strip_width, int32_t scroll, int32_t arrow) {
    TabMeasure measured;
    if (arrow < 1)
        arrow = 1;
    if (strip_width <= arrow * 2 || total <= strip_width) {
        measured.visible = std::max(strip_width, int32_t{0});
        return measured;
    }
    measured.left = scroll > 0;
    int32_t visible = strip_width - (measured.left ? arrow : 0);
    if (total > scroll + visible) {
        const int32_t narrower = visible - arrow;
        if (narrower > 0 && total > scroll + narrower) {
            visible = narrower;
            measured.right = true;
        }
    }
    if (visible < 1)
        visible = 1;
    measured.visible = visible;
    measured.max_scroll = std::max(total - visible, int32_t{0});
    return measured;
}

/// Clamps a scroll to the strip and, when asked, so the open tab lies
/// wholly in the view. The rectangles' x is each tab's left in the unshifted
/// content, from 0.
///
/// @param asked the scroll asked for, in points
/// @param total the tabs' width, in points
/// @param strip_width the strip's width, in points
/// @param arrow an arrow's width, in points
/// @param content each tab, from the content's left
/// @param selected the open tab's index
/// @param keep_selected true keeps the open tab wholly in the view
/// @return the clamped scroll, in points
int32_t fit_tab_scroll(
    int32_t asked,
    int32_t total,
    int32_t strip_width,
    int32_t arrow,
    const std::vector<Rect>& content,
    std::size_t selected,
    bool keep_selected
) {
    int32_t scroll = std::max(asked, int32_t{0});
    for (int32_t pass = 0; pass < 8; ++pass) {
        const TabMeasure measured = measure_strip(total, strip_width, scroll, arrow);
        int32_t next = std::clamp(scroll, int32_t{0}, measured.max_scroll);
        if (keep_selected && selected < content.size()) {
            const Rect& tab = content[selected];
            int32_t low = tab.x + tab.width - measured.visible;
            if (tab.width > measured.visible)
                low = tab.x;
            const int32_t high = tab.x;
            if (low > high)
                low = high;
            const int32_t floor = std::max(low, int32_t{0});
            const int32_t ceiling = std::min(high, measured.max_scroll);
            if (floor <= ceiling)
                next = std::clamp(scroll, floor, ceiling);
            else
                next = std::clamp(tab.x, int32_t{0}, measured.max_scroll);
        }
        if (next == scroll)
            break;
        scroll = next;
    }
    return scroll;
}

/// Draws one chevron, centred in an arrow above the strip's rule.
///
/// @param canvas where it is drawn
/// @param rect the arrow
/// @param pointing_left a left chevron; a right one otherwise
/// @param ink the colour
void draw_tab_arrow(const Canvas& canvas, const Rect& rect, bool pointing_left, Colour ink) {
    if (rect.width <= 0 || rect.height <= 0)
        return;
    const int32_t x = rect.x + (rect.width - kChevron) / 2;
    const int32_t y = rect.y + (rect.height - kMetrics.tab_rule - kChevron) / 2;
    const Point at{x, y};
    if (pointing_left)
        draw_mark(canvas, kTabArrowLeft, at, ink);
    else
        draw_mark(canvas, Mark::arrow_closed, at, ink);
}

/// Moves one step and clamps it to the strip. A positive direction reveals
/// the next tab whose right lies past the view; a negative one the last
/// whose left lies before the scroll. The open tab may leave the view.
///
/// @param scroll the scroll now, in points
/// @param total the tabs' width, in points
/// @param strip_width the strip's width, in points
/// @param arrow an arrow's width, in points
/// @param content each tab, from the content's left
/// @param direction positive toward the right, negative toward the left
/// @return the scroll after the step, in points
int32_t step_tab_scroll(
    int32_t scroll,
    int32_t total,
    int32_t strip_width,
    int32_t arrow,
    const std::vector<Rect>& content,
    int32_t direction
) {
    const TabMeasure measured = measure_strip(total, strip_width, scroll, arrow);
    int32_t asked = scroll;
    if (direction > 0) {
        const int32_t edge = scroll + measured.visible;
        for (const Rect& tab : content) {
            if (tab.x + tab.width > edge) {
                asked = tab.x + tab.width - measured.visible;
                break;
            }
        }
    } else if (direction < 0) {
        for (const Rect& tab : content)
            if (tab.x < scroll)
                asked = tab.x;
    }
    return fit_tab_scroll(asked, total, strip_width, arrow, content, 0, false);
}

} // namespace

TabStrip place_tabs(const Fonts& fonts, const Rect& strip, const TabsLook& look) {
    TabStrip placed;
    placed.strip = strip;
    placed.tabs = tab_rects(fonts, strip, look);
    placed.selected = look.selected < look.captions.size() ? look.selected : 0;
    int32_t total = 0;
    if (!placed.tabs.empty()) {
        const Rect& last = placed.tabs.back();
        total = last.x + last.width - strip.x;
    }
    std::vector<Rect> content = placed.tabs;
    for (Rect& tab : content)
        tab.x -= strip.x;
    placed.scroll = fit_tab_scroll(
        look.scroll,
        total,
        strip.width,
        kTabArrow,
        content,
        placed.selected,
        look.keep_selected_in_view
    );
    const TabMeasure measured = measure_strip(total, strip.width, placed.scroll, kTabArrow);
    placed.overflows = total > strip.width && strip.width > kTabArrow * 2;
    placed.limit = measured.max_scroll;
    const int32_t inset = measured.left ? kTabArrow : 0;
    placed.view = {strip.x + inset, strip.y, measured.visible, strip.height};
    if (measured.left)
        placed.left_arrow = {strip.x, strip.y, kTabArrow, strip.height};
    if (measured.right)
        placed.right_arrow = {strip.x + strip.width - kTabArrow, strip.y, kTabArrow, strip.height};
    // Content at `scroll` is the view's left, past a left arrow.
    for (Rect& tab : placed.tabs)
        tab.x += inset - placed.scroll;
    return placed;
}

void draw_tabs(const Canvas& canvas, const Rect& strip, const TabsLook& look) {
    if (!drawable(canvas))
        return;
    draw_rule(canvas, strip.x, strip.y + strip.height - kMetrics.hairline, strip.width);
    if (canvas.fonts == nullptr)
        return;
    const TabStrip placed = place_tabs(*canvas.fonts, strip, look);
    if (placed.left_arrow.width > 0)
        draw_tab_arrow(
            canvas, placed.left_arrow, true, look.left_arrow_hovered ? colour::text : colour::hint
        );
    if (placed.right_arrow.width > 0)
        draw_tab_arrow(
            canvas,
            placed.right_arrow,
            false,
            look.right_arrow_hovered ? colour::text : colour::hint
        );
    const Canvas room = clipped(canvas, placed.view);
    const int32_t side = tab_side_padding(look);
    const int32_t letter_gap = tab_letter_gap(look);
    for (std::size_t index = 0; index < placed.tabs.size() && index < look.captions.size();
         ++index) {
        const Rect& tab = placed.tabs[index];
        const bool selected = index == look.selected;
        const bool hovered = look.hovered.has_value() && *look.hovered == index;
        const std::string& caption = look.captions[index];
        const int32_t caption_columns = caption_width(canvas.fonts, caption, letter_gap);
        const int32_t above_rule = tab.height - kMetrics.tab_rule;
        line(
            room,
            FontRole::small,
            caption,
            {tab.x + side, tab.y, caption_columns, above_rule},
            Align::left,
            selected || hovered ? colour::text : colour::text_secondary,
            letter_gap
        );
        const int32_t count = count_of(look, index);
        if (look.count_style == TabCountStyle::words) {
            if (count >= 0) {
                const std::string digits = std::to_string(count);
                const int32_t digits_width = text_width(*canvas.fonts, FontRole::small, digits);
                const Rect digits_box{
                    tab.x + side + caption_columns + kMetrics.tab_count_gap,
                    tab.y,
                    digits_width,
                    above_rule,
                };
                const bool amber =
                    index < look.count_amber.size() && look.count_amber[index] != 0 && count > 0;
                Colour ink = selected || hovered ? colour::text : colour::text_secondary;
                if (amber)
                    ink = colour::lock;
                line(room, FontRole::small, digits, digits_box, Align::left, ink);
            }
        } else if (count > 0) {
            const Rect pill{
                tab.x + side + caption_columns + kMetrics.tab_count_gap,
                tab.y + (above_rule - kMetrics.tab_count_height) / 2,
                count_width(canvas.fonts, count),
                kMetrics.tab_count_height,
            };
            fill_round_rect(room, pill, std::min(pill.width, pill.height) / 2, colour::lock);
            line(
                room, FontRole::small, std::to_string(count), pill, Align::centre, colour::on_accent
            );
        }
        if (selected)
            fill_round_rect(
                room,
                {tab.x, tab.y + above_rule, tab.width, kMetrics.tab_rule},
                kMetrics.tab_rule / 2,
                colour::accent
            );
    }
}

void add_tabs(
    DisplayList& list,
    const Fonts& fonts,
    const Rect& strip,
    const TabsLook& look,
    std::span<const ControlId> ids,
    std::span<const std::string> names,
    ControlId left_id,
    std::string left_name,
    ControlId right_id,
    std::string right_name
) {
    const TabStrip placed = place_tabs(fonts, strip, look);
    TabsLook stored = look;
    stored.scroll = placed.scroll;
    stored.view_x = placed.view.x;
    stored.view_width = placed.view.width;

    Item item;
    item.role = Role::tabs;
    item.rect = strip;
    if (look.selected < look.captions.size())
        item.text = look.captions[look.selected];
    item.look = std::move(stored);
    list.items.push_back(std::move(item));

    for (std::size_t index = 0; index < placed.tabs.size(); ++index) {
        std::string text;
        if (index < look.readings.size() && !look.readings[index].empty()) {
            text = look.readings[index];
        } else if (index < look.captions.size()) {
            text = look.captions[index];
            const int32_t count = count_of(look, index);
            if (count > 0)
                text += " " + std::to_string(count);
        }
        add_control(
            list,
            placed.tabs[index],
            index < ids.size() ? ids[index] : no_control,
            index < names.size() ? names[index] : std::string{},
            ControlKind::tab,
            false,
            index == look.selected,
            std::move(text),
            true
        );
        if (!wholly_in(placed.tabs[index], placed.view))
            list.controls.back().clip = placed.view;
    }
    const auto add_arrow = [&](const Rect& rect, ControlId id, std::string name, std::string text) {
        if (rect.width <= 0 || id == no_control)
            return;
        add_control(
            list,
            rect,
            id,
            std::move(name),
            ControlKind::button,
            false,
            false,
            std::move(text),
            true
        );
        list.controls.back().focusable = false;
    };
    add_arrow(placed.left_arrow, left_id, std::move(left_name), "scroll left");
    add_arrow(placed.right_arrow, right_id, std::move(right_name), "scroll right");
}

namespace {

/// A strip of tabs as a display list holds it: its tabs from the content's
/// left, their width, its arrow's width and its scroll.
struct ListedStrip {
    Rect strip{};                ///< the strip
    std::vector<Rect> content{}; ///< each tab, from the content's left, left to right
    int32_t total{};             ///< the tabs' width, in points
    int32_t arrow{kTabArrow};    ///< an arrow's width, in points
    int32_t scroll{};            ///< the scroll the strip was placed at, in points
};

/// Reads the strip a list holds: its tabs item and the tab controls on its
/// row.
///
/// @param list the display list a strip was added to
/// @return the strip; none when the list has none, or it was not placed or has no tabs
std::optional<ListedStrip> listed_strip(const DisplayList& list) {
    const Item* tabs_item = nullptr;
    for (const Item& item : list.items) {
        if (item.role == Role::tabs) {
            tabs_item = &item;
            break;
        }
    }
    if (tabs_item == nullptr)
        return std::nullopt;
    const auto* look = std::get_if<TabsLook>(&tabs_item->look);
    if (look == nullptr || look->view_width <= 0)
        return std::nullopt;

    ListedStrip listed;
    listed.strip = tabs_item->rect;
    listed.scroll = look->scroll;
    const Rect& strip = listed.strip;
    for (const Control& control : list.controls) {
        if (control.kind != ControlKind::tab)
            continue;
        if (control.rect.y != strip.y || control.rect.height != strip.height)
            continue;
        const int32_t left = control.rect.x - look->view_x + look->scroll;
        listed.content.push_back(Rect{left, 0, control.rect.width, control.rect.height});
    }
    if (listed.content.empty())
        return std::nullopt;
    std::sort(
        listed.content.begin(), listed.content.end(), [](const Rect& ahead, const Rect& behind) {
            return ahead.x < behind.x;
        }
    );
    listed.total = listed.content.back().x + listed.content.back().width;
    const int32_t left_inset = look->view_x - strip.x;
    const int32_t right_inset = strip.x + strip.width - (look->view_x + look->view_width);
    if (left_inset > 0)
        listed.arrow = left_inset;
    else if (right_inset > 0)
        listed.arrow = right_inset;
    return listed;
}

} // namespace

int32_t tab_scroll_step(const DisplayList& list, int32_t steps) {
    const std::optional<ListedStrip> listed = listed_strip(list);
    if (!listed) {
        for (const Item& item : list.items)
            if (const auto* look = std::get_if<TabsLook>(&item.look);
                item.role == Role::tabs && look != nullptr)
                return look->scroll;
        return 0;
    }
    const int32_t width = listed->strip.width;
    int32_t scroll = listed->scroll;
    int32_t remaining = steps;
    while (remaining > 0) {
        scroll = step_tab_scroll(scroll, listed->total, width, listed->arrow, listed->content, 1);
        --remaining;
    }
    while (remaining < 0) {
        scroll = step_tab_scroll(scroll, listed->total, width, listed->arrow, listed->content, -1);
        ++remaining;
    }
    return scroll;
}

int32_t tab_scroll_by(const DisplayList& list, int32_t from, int32_t points) {
    const std::optional<ListedStrip> listed = listed_strip(list);
    if (!listed)
        return 0;
    const int64_t asked = static_cast<int64_t>(from) + points;
    const auto clamped =
        static_cast<int32_t>(std::clamp(asked, int64_t{0}, static_cast<int64_t>(listed->total)));
    return fit_tab_scroll(
        clamped, listed->total, listed->strip.width, listed->arrow, listed->content, 0, false
    );
}

std::optional<Rect> tab_drag_area(const DisplayList& list) {
    for (const Item& item : list.items) {
        if (item.role != Role::tabs)
            continue;
        const auto* look = std::get_if<TabsLook>(&item.look);
        if (look == nullptr || look->view_width <= 0 || look->view_width >= item.rect.width)
            return std::nullopt;
        return Rect{look->view_x, item.rect.y, look->view_width, item.rect.height};
    }
    return std::nullopt;
}

// ---- Cards ----

int32_t card_height(int32_t width) noexcept {
    const int32_t side = std::max(width - 2 * kMetrics.card_inset, int32_t{0});
    return side + kMetrics.regular_line + kMetrics.small_line + 3 * kMetrics.card_inset;
}

std::vector<Rect> card_cells(const Rect& area, std::size_t count, std::size_t columns) {
    std::vector<Rect> cells;
    if (count == 0)
        return cells;
    const int64_t gap = kMetrics.card_gap;
    // No more columns than the area has points, so a column is never empty.
    const int64_t most = std::max<int64_t>(area.width, 1);
    int64_t wanted =
        columns > static_cast<std::size_t>(most) ? most : static_cast<int64_t>(columns);
    if (wanted == 0)
        wanted = std::max<int64_t>((area.width + gap) / (kMetrics.card_least_width + gap), 1);
    // The least width at which exactly the wanted columns fit the area.
    const auto least = static_cast<int32_t>((area.width + gap) / wanted - gap);
    const std::vector<Rect> first_row =
        grid(area, least, 0, kMetrics.card_gap, static_cast<std::size_t>(wanted));
    if (first_row.empty())
        return cells;
    // The left columns are the widest, so the first cell's height spaces the rows.
    const int32_t row_step = card_height(first_row.front().width) + kMetrics.card_gap;
    cells.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        Rect cell = first_row[index % first_row.size()];
        cell.y = area.y + static_cast<int32_t>(index / first_row.size()) * row_step;
        cell.height = card_height(cell.width);
        cells.push_back(cell);
    }
    return cells;
}

void draw_card(const Canvas& canvas, const Rect& rect, const CardLook& look) {
    if (!drawable(canvas))
        return;
    Colour border = colour::control_border;
    if (look.selected)
        border = colour::accent;
    else if (look.hovered)
        border = colour::control_hover;
    const int32_t radius = std::min(kMetrics.card_radius, std::min(rect.width, rect.height) / 4);
    draw_capsule_corners(
        canvas, rect, radius, look.hovered ? colour::list_selected : colour::hover, border
    );

    const Rect preview = card_preview(rect);
    if (renderer::picture_drawable(look.picture)) {
        // The map's own shape, as large as the square allows, centred on black.
        Rect fitted = preview;
        const int64_t picture_width = look.picture.width;
        const int64_t picture_height = look.picture.height;
        if (picture_width * preview.height >= picture_height * preview.width) {
            fitted.height = static_cast<int32_t>(
                std::max<int64_t>(1, picture_height * preview.width / picture_width)
            );
            fitted.y += (preview.height - fitted.height) / 2;
        } else {
            fitted.width = static_cast<int32_t>(
                std::max<int64_t>(1, picture_width * preview.height / picture_height)
            );
            fitted.x += (preview.width - fitted.width) / 2;
        }
        fill(canvas, preview, Colour{0, 0, 0});
        draw_picture(canvas, fitted, look.picture);
    } else if (preview.width > 0) {
        fill(canvas, preview, colour::well);
        cut_line(
            clipped(canvas, preview),
            FontRole::small,
            look.placeholder,
            preview,
            Align::centre,
            colour::hint
        );
    }
    draw_chip_line(
        canvas,
        look.chips,
        {preview.x + kMetrics.card_inset,
         preview.y + preview.height - kMetrics.card_inset - kMetrics.chip_height},
        preview.x + preview.width - kMetrics.card_inset
    );

    const int32_t text_width_room = std::max(rect.width - 2 * kMetrics.card_inset, int32_t{0});
    const Rect title{
        rect.x + kMetrics.card_inset,
        preview.y + preview.height + kMetrics.card_inset,
        text_width_room,
        kMetrics.regular_line,
    };
    cut_line(canvas, FontRole::regular, look.title, title, Align::left, colour::text);
    const Rect subtitle{title.x, title.y + title.height, text_width_room, kMetrics.small_line};
    cut_line(canvas, FontRole::small, look.subtitle, subtitle, Align::left, colour::hint);

    if (look.greyed)
        blend_corner_rect(canvas, rect, radius, colour::panel, locked_fade);
}

void add_card(
    DisplayList& list, const Rect& rect, const CardLook& look, ControlId id, std::string name
) {
    Item item;
    item.role = Role::card;
    item.rect = rect;
    item.control = id;
    item.text = look.title;
    item.state.hovered = look.hovered;
    item.state.selected = look.selected;
    item.look = look;
    list.items.push_back(std::move(item));
    add_control(
        list,
        rect,
        id,
        std::move(name),
        ControlKind::list_item,
        false,
        look.selected,
        look.title,
        true
    );
}

// ---- Hover cards ----

namespace {

/// How far a line's letters may reach past its width at the right, in
/// points: the game fonts' last column of a letter's edge. A hover card keeps
/// it inside its right padding, so the room after the last letter matches the
/// room before the first.
constexpr int32_t kLetterReach = 1;

} // namespace

Point hover_card_size(const Fonts& fonts, const HoverCardLook& look) {
    const int32_t padding = kMetrics.hover_card_padding;
    int32_t widest = 0;
    int32_t lines = 0;
    if (has_header(look)) {
        int32_t header = 0;
        const auto join = [&](int32_t part) { header += (header > 0 ? padding : 0) + part; };
        if (look.mark)
            join(mark_size(Mark::oa_small).x);
        if (!look.title.empty())
            join(text_width(fonts, FontRole::small, look.title));
        if (!look.aside.empty())
            join(text_width(fonts, FontRole::small, look.aside));
        widest = header;
        ++lines;
    }
    const int32_t values = label_column(&fonts, look);
    for (const HoverCardRow& row : look.rows) {
        const int32_t from = row.label.empty() ? 0 : values;
        widest = std::max(widest, from + text_width(fonts, FontRole::small, row.value));
        ++lines;
    }
    for (const std::string& text : look.block) {
        widest = std::max(widest, text_width(fonts, FontRole::small, text));
        ++lines;
    }
    const int32_t width = look.width > 0 ? look.width : widest + 2 * padding + kLetterReach;
    return {width, lines * kMetrics.small_line + 2 * padding};
}

Rect hover_card_rect(const Rect& anchor, const Rect& frame, Point size) noexcept {
    const int32_t frame_right = frame.x + frame.width;
    const int32_t frame_bottom = frame.y + frame.height;
    const int32_t x = std::max(std::min(anchor.x, frame_right - size.x), frame.x);
    const int32_t below = anchor.y + anchor.height + kMetrics.hover_card_arrow;
    const int32_t above = anchor.y - kMetrics.hover_card_arrow - size.y;
    int32_t y = below;
    if (below + size.y > frame_bottom)
        y = above >= frame.y ? above : frame_bottom - size.y;
    y = std::max(std::min(y, frame_bottom - size.y), frame.y);
    return {x, y, size.x, size.y};
}

void draw_hover_card(const Canvas& canvas, const Rect& rect, const HoverCardLook& look) {
    if (!drawable(canvas))
        return;
    draw_capsule_corners(
        canvas,
        rect,
        std::min(kMetrics.card_radius, std::min(rect.width, rect.height) / 4),
        colour::band,
        colour::strong_edge
    );
    draw_arrow(canvas, rect, look);

    const int32_t padding = kMetrics.hover_card_padding;
    const int32_t row_height = kMetrics.small_line;
    const int32_t left = rect.x + padding;
    const int32_t right = rect.x + rect.width - padding - kLetterReach;
    const int32_t room = std::max(right - left, int32_t{0});
    int32_t row = rect.y + padding;

    if (has_header(look)) {
        int32_t pen = left;
        if (look.mark) {
            const Point mark = mark_size(Mark::oa_small);
            // The logo, when the canvas carries it, in a square as wide as the
            // one-bit mark, so the header keeps that mark's width. Otherwise
            // the one-bit mark.
            if (renderer::picture_drawable(canvas.icon)) {
                const int32_t side = std::max(mark.x, mark.y);
                draw_picture(canvas, {pen, row + (row_height - side) / 2, side, side}, canvas.icon);
            } else {
                draw_mark(
                    canvas, Mark::oa_small, {pen, row + (row_height - mark.y) / 2}, colour::brand
                );
            }
            pen += mark.x + padding;
        }
        int32_t aside_width = 0;
        if (!look.aside.empty()) {
            // The wider of the two measures, as a list row's aside is, so
            // "built in" is not cut when the canvas's measure is the narrower.
            aside_width = std::min(
                whole_width(canvas, FontRole::small, look.aside), std::max(right - pen, int32_t{0})
            );
            cut_line(
                canvas,
                FontRole::small,
                look.aside,
                {right - aside_width, row, aside_width, row_height},
                Align::right,
                colour::hint
            );
        }
        const int32_t title_right = aside_width > 0 ? right - aside_width - padding : right;
        cut_line(
            canvas,
            FontRole::small,
            look.title,
            {pen, row, std::max(title_right - pen, int32_t{0}), row_height},
            Align::left,
            colour::text
        );
        row += row_height;
    }

    const int32_t values = label_column(canvas.fonts, look);
    for (const HoverCardRow& entry : look.rows) {
        int32_t value_left = left;
        if (!entry.label.empty()) {
            cut_line(
                canvas,
                FontRole::small,
                entry.label,
                {left, row, std::max(values - padding, int32_t{0}), row_height},
                Align::left,
                colour::hint
            );
            value_left = left + values;
        }
        cut_line(
            canvas,
            FontRole::small,
            entry.value,
            {value_left, row, std::max(right - value_left, int32_t{0}), row_height},
            Align::left,
            entry.value_colour
        );
        row += row_height;
    }

    for (const std::string& text : look.block) {
        cut_line(
            canvas, FontRole::small, text, {left, row, room, row_height}, Align::left, colour::quiet
        );
        row += row_height;
    }
}

void add_hover_card(DisplayList& list, const Rect& rect, const HoverCardLook& look) {
    Item item;
    item.role = Role::hover_card;
    item.rect = rect;
    item.text = look.title;
    item.look = look;
    list.items.push_back(std::move(item));
}

bool hover_card_due(HoverTimer& timer, ControlId hovered, uint64_t now_ms) noexcept {
    if (hovered == no_control) {
        timer = {};
        return false;
    }
    if (hovered != timer.over) {
        timer.over = hovered;
        timer.since_ms = now_ms;
        return false;
    }
    return now_ms >= timer.since_ms && now_ms - timer.since_ms >= hover_card_delay_ms;
}

// ---- Links ----

int32_t link_width(const Fonts& fonts, const LinkLook& look) {
    return text_width(fonts, FontRole::small, look.caption);
}

void draw_link(const Canvas& canvas, const Rect& rect, const LinkLook& look) {
    if (!drawable(canvas))
        return;
    Colour ink = colour::accent;
    if (!look.enabled)
        ink = colour::switch_idle;
    else if (look.hovered)
        ink = colour::accent_light;
    line(canvas, FontRole::small, look.caption, rect, Align::left, ink);
}

void add_link(
    DisplayList& list, const Rect& rect, const LinkLook& look, ControlId id, std::string name
) {
    Item item;
    item.role = Role::link;
    item.rect = rect;
    item.control = id;
    item.text = look.caption;
    item.state.hovered = look.hovered;
    item.state.disabled = !look.enabled;
    item.look = look;
    list.items.push_back(std::move(item));
    add_control(
        list, rect, id, std::move(name), ControlKind::link, false, false, look.caption, look.enabled
    );
}

void draw_badge(const Canvas& canvas, const Rect& rect, const BadgeLook& look) {
    if (!drawable(canvas) || rect.width <= 0 || rect.height <= 0)
        return;
    if (renderer::picture_drawable(look.picture)) {
        draw_picture(canvas, rect, look.picture);
        outline(canvas, rect, colour::control_border);
        return;
    }
    if (look.tag.has_value()) {
        const int32_t side = std::min(rect.width, rect.height);
        const int32_t icon_side = side * 70 / 100;
        const Rect icon{
            rect.x + (rect.width - icon_side) / 2,
            rect.y + (rect.height - icon_side) / 2,
            icon_side,
            icon_side,
        };
        fill(canvas, rect, look.face);
        draw_tag_icon(canvas, icon, *look.tag, colour::on_accent);
        return;
    }
    if (!look.letters.empty()) {
        fill(canvas, rect, look.face);
        // A badge keeps as many letters as fit its square, from the start,
        // and drops the rest. No ellipsis: the square has no room for one.
        std::string_view letters = look.letters;
        while (!letters.empty() && measure_text(canvas, FontRole::small, letters) > rect.width) {
            std::size_t end = letters.size();
            do {
                --end;
            } while (end > 0 && continuation_byte(letters[end]));
            letters.remove_suffix(letters.size() - end);
        }
        if (!letters.empty())
            line(
                clipped(canvas, rect),
                FontRole::small,
                letters,
                rect,
                Align::centre,
                colour::on_accent
            );
        return;
    }
    const int32_t hairline = kMetrics.hairline;
    const int32_t period = kMetrics.badge_dash + kMetrics.badge_dash_gap;
    for (int32_t along = 0; along < rect.width; along += period) {
        const int32_t dash = std::min(kMetrics.badge_dash, rect.width - along);
        fill(canvas, {rect.x + along, rect.y, dash, hairline}, colour::control_border);
        fill(
            canvas,
            {rect.x + along, rect.y + rect.height - hairline, dash, hairline},
            colour::control_border
        );
    }
    for (int32_t along = 0; along < rect.height; along += period) {
        const int32_t dash = std::min(kMetrics.badge_dash, rect.height - along);
        fill(canvas, {rect.x, rect.y + along, hairline, dash}, colour::control_border);
        fill(
            canvas,
            {rect.x + rect.width - hairline, rect.y + along, hairline, dash},
            colour::control_border
        );
    }
}

void add_badge(DisplayList& list, const Rect& rect, const BadgeLook& look) {
    Item item;
    item.role = Role::package_badge;
    item.rect = rect;
    item.text = look.letters;
    item.look = look;
    list.items.push_back(std::move(item));
}

namespace {

/// The most pixels across or down a drag handle is drawn at.
constexpr int32_t kMostGripPixels = 1024;

/// The samples across, and down, each pixel of a drag handle.
constexpr int32_t kGripSamples = 4;

/// Returns a drag handle's colour. Disabled wins, then held, then hovered.
///
/// @param look the handle
/// @return its colour
Colour grip_colour(const GripLook& look) noexcept {
    if (look.disabled)
        return colour::control_hover;
    if (look.held)
        return colour::accent;
    if (look.hovered)
        return colour::text;
    return colour::quiet;
}

/// Returns where a half-point inside a rectangle falls, in eighths of a
/// pixel from the rectangle's pixel box.
///
/// @param half_from_left the place, in half-points from the rectangle's left or top
/// @param anchor the rectangle's left or top, in points, plus a smooth canvas's origin
/// @param origin_px that edge in canvas pixels
/// @param percent the scale, in percent
/// @return the place, in eighths of a pixel from the box's top or left
int64_t
grip_eighths(int32_t half_from_left, int32_t anchor, int32_t origin_px, int32_t percent) noexcept {
    const int64_t half_absolute = static_cast<int64_t>(2) * anchor + half_from_left;
    return half_absolute * percent / 25 - static_cast<int64_t>(origin_px) * 8;
}

} // namespace

void draw_grip(const Canvas& canvas, const Rect& rect, const GripLook& look) {
    if (!drawable(canvas) || rect.width <= 0 || rect.height <= 0)
        return;
    const bool smooth = canvas.smooth.painter != nullptr;
    const int32_t percent =
        smooth ? canvas.smooth.scale_percent : std::max(canvas.placement.scale, int32_t{1}) * 100;
    if (percent <= 0)
        return;
    const Point pixels = covered_pixels(canvas, rect);
    if (pixels.x <= 0 || pixels.y <= 0 || pixels.x > kMostGripPixels || pixels.y > kMostGripPixels)
        return;
    const int32_t anchor_x = smooth ? canvas.smooth.origin.x + rect.x : 0;
    const int32_t anchor_y = smooth ? canvas.smooth.origin.y + rect.y : 0;
    const int32_t left_px = to_canvas({anchor_x, 0, 0, 0}, percent).x;
    const int32_t top_px = to_canvas({0, anchor_y, 0, 0}, percent).y;
    const int64_t radius = static_cast<int64_t>(2) * percent / 25;
    const int64_t radius_squared = radius * radius;
    constexpr int32_t kColumnHalf[] = {-3, 3};
    constexpr int32_t kRowHalf[] = {-9, -3, 3, 9};
    int64_t centres_x[2];
    int64_t centres_y[4];
    for (int32_t column = 0; column < 2; ++column)
        centres_x[column] =
            grip_eighths(rect.width + kColumnHalf[column], anchor_x, left_px, percent);
    for (int32_t row = 0; row < 4; ++row)
        centres_y[row] = grip_eighths(rect.height + kRowHalf[row], anchor_y, top_px, percent);
    std::vector<uint8_t> coverage(
        static_cast<std::size_t>(pixels.x) * static_cast<std::size_t>(pixels.y), 0
    );
    constexpr int64_t samples = static_cast<int64_t>(kGripSamples) * kGripSamples;
    for (int32_t row = 0; row < pixels.y; ++row) {
        for (int32_t column = 0; column < pixels.x; ++column) {
            int64_t inside = 0;
            for (int32_t down = 0; down < kGripSamples; ++down) {
                for (int32_t across = 0; across < kGripSamples; ++across) {
                    const int64_t sample_x = static_cast<int64_t>(8) * column + 2 * across + 1;
                    const int64_t sample_y = static_cast<int64_t>(8) * row + 2 * down + 1;
                    bool covered = false;
                    for (const int64_t centre_x : centres_x) {
                        for (const int64_t centre_y : centres_y) {
                            const int64_t across_dot = sample_x - centre_x;
                            const int64_t down_dot = sample_y - centre_y;
                            if (across_dot * across_dot + down_dot * down_dot <= radius_squared) {
                                covered = true;
                                break;
                            }
                        }
                        if (covered)
                            break;
                    }
                    if (covered)
                        ++inside;
                }
            }
            coverage
                [static_cast<std::size_t>(row) * static_cast<std::size_t>(pixels.x) +
                 static_cast<std::size_t>(column)] =
                    static_cast<uint8_t>((inside * 255 + samples / 2) / samples);
        }
    }
    blend_coverage(canvas, rect, coverage, grip_colour(look));
}

void add_grip(
    DisplayList& list, const Rect& rect, const GripLook& look, ControlId id, std::string name
) {
    Item item;
    item.role = Role::grip;
    item.rect = rect;
    item.control = id;
    item.state.hovered = look.hovered;
    item.state.pressed = look.held;
    item.state.disabled = look.disabled;
    item.look = look;
    list.items.push_back(std::move(item));
    if (id == no_control)
        return;
    add_control(
        list, rect, id, std::move(name), ControlKind::button, false, false, {}, !look.disabled
    );
}

} // namespace oa::ui::kit
