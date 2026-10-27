// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A notice and a question: their text wrapped and placed, their display
// lists, what pointer, finger and key events do to them, and their drawing.
// A work dialog is the same panel with one line and a bar or a spinner, and
// no buttons. The arithmetic is the settings dialog's notices' and prompts',
// moved here unchanged but for a question's buttons, which also fit their
// captions in the fonts; a box's width and greatest height are its size class's.

#include "oa/ui/kit/components_more.hpp"

#include "oa/ui/kit/canvas.hpp"
#include "oa/ui/kit/chrome.hpp"
#include "oa/ui/kit/controls.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/paint/painter.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::ui::kit {

namespace {

/// The Compact metrics every notice and question is placed by, but for its
/// width and its greatest height, which are its size class's.
constexpr const Metrics& metrics = compact_metrics;

/// The text's first column.
constexpr int32_t text_left = compact_metrics.padding;

/// Returns a box's width at its size class.
///
/// @param size_class the class
/// @return the width, in points
int32_t box_width(SizeClass size_class) noexcept {
    return metrics_of(size_class).notice_width;
}

/// Returns the text's width in a box.
///
/// @param width the box's width
/// @return the text's width, in points
int32_t line_width_in(int32_t width) noexcept {
    return width - 2 * metrics.padding;
}

/// Returns the width inside a box's edge.
///
/// @param width the box's width
/// @return the width, in points
int32_t inner_width_in(int32_t width) noexcept {
    return width - 2 * metrics.edge;
}

/// The text's lines and the row under the last of them.
struct PlacedText {
    std::vector<PlacedLine> lines{}; ///< every line, top to bottom, before any is cut
    int32_t bottom{};                ///< the row under the last line
};

/// Places paragraphs and a failure under the header: paths in the regular
/// font broken at their separators, the rest in the small font with a new
/// line starting a line and the rest broken between words, paragraph_gap
/// rows between paragraphs.
///
/// @param paragraphs the text
/// @param failure the failure line's text; empty for none
/// @param regular a text's width in the regular font
/// @param small a text's width in the small font
/// @param width the box's width
/// @return the lines and the row under them
PlacedText place_text(
    const std::vector<Paragraph>& paragraphs,
    std::string_view failure,
    const Measure& regular,
    const Measure& small,
    int32_t width
) {
    const int32_t line_width = line_width_in(width);
    PlacedText placed{};
    int32_t row = metrics.text_top;
    bool first = true;
    const auto add = [&](const std::vector<std::string>& wrapped, bool path, bool is_failure) {
        if (!first)
            row += metrics.paragraph_gap;
        first = false;
        const int32_t line_height = path ? metrics.regular_line : metrics.small_line;
        for (const auto& text : wrapped) {
            placed.lines.push_back(
                PlacedLine{text, path, is_failure, {text_left, row, line_width, line_height}}
            );
            row += line_height;
        }
    };
    for (const auto& paragraph : paragraphs) {
        if (paragraph.path) {
            add(wrap_path(paragraph.text, line_width, regular), true, false);
            continue;
        }
        // A new line is its own line, so a list of notes does not run together.
        WrapRules rules;
        rules.newlines = true;
        add(wrap(paragraph.text, line_width, small, rules), false, false);
    }
    if (!failure.empty())
        add(wrap(failure, line_width, small), false, true);
    placed.bottom = row;
    return placed;
}

/// Returns the height of a box whose text ends at a row: its text, the gap
/// under it and the footer, from its class's least to its greatest height.
///
/// @param text_bottom the row under the text
/// @param size_class the box's size class
/// @return the height
int32_t height_for(int32_t text_bottom, SizeClass size_class) noexcept {
    const Metrics& sized = metrics_of(size_class);
    return std::clamp(
        text_bottom + metrics.text_bottom_gap + 1 + metrics.footer_height + metrics.edge,
        sized.least_notice_height,
        sized.greatest_notice_height
    );
}

/// Returns the row of the line over a box's footer.
///
/// @param height the box's height
/// @return the row
int32_t footer_rule_of(int32_t height) noexcept {
    return height - metrics.edge - metrics.footer_height - 1;
}

/// Returns the top row of a box's footer buttons.
///
/// @param height the box's height
/// @return the row
int32_t button_top_of(int32_t height) noexcept {
    return footer_rule_of(height) + 1 + (metrics.footer_height - metrics.button_height) / 2;
}

/// Tells whether a rectangle's bottom lies above the footer's rule by half
/// the gap kept under the text.
///
/// @param rect the rectangle
/// @param footer_rule the row of the line over the footer
/// @return true when it fits
bool fits_over(const Rect& rect, int32_t footer_rule) noexcept {
    return rect.y + rect.height <= footer_rule - metrics.text_bottom_gap / 2;
}

/// Returns the lines of placed text a box's height does not cut.
///
/// @param lines the lines
/// @param footer_rule the row of the line over the footer
/// @return the lines that fit
std::vector<PlacedLine> lines_that_fit(std::vector<PlacedLine> lines, int32_t footer_rule) {
    std::vector<PlacedLine> fitting;
    for (auto& line : lines)
        if (fits_over(line.rect, footer_rule))
            fitting.push_back(std::move(line));
    return fitting;
}

/// Returns the title's place in the header.
///
/// @param width the box's width
/// @return the rectangle, in points
Rect title_rect(int32_t width) noexcept {
    const int32_t title_left = notice_mark.x + notice_mark.width + metrics.header_gap;
    return {title_left, metrics.edge, width - metrics.padding - title_left, metrics.header_height};
}

/// OK and the open button, placed.
struct NoticeButtons {
    Rect ok{};   ///< OK, at the footer's right
    Rect open{}; ///< the open button, before OK
};

/// Tells whether a notice draws the button that opens a folder.
///
/// @param notice the notice
/// @return true when its open caption is not empty
bool notice_opens(const Notice& notice) noexcept {
    return !notice.open_caption.empty();
}

/// Returns a notice's buttons, which depend on its height and its width alone.
///
/// @param height the notice's height
/// @param width the notice's width
/// @return the buttons' rectangles
NoticeButtons notice_buttons(int32_t height, int32_t width) noexcept {
    const int32_t top = button_top_of(height);
    const Rect ok{
        width - metrics.padding - metrics.button_width,
        top,
        metrics.button_width,
        metrics.button_height
    };
    return {
        ok,
        {ok.x - metrics.button_gap - metrics.open_width,
         top,
         metrics.open_width,
         metrics.button_height}
    };
}

/// Returns a text's width in a font, or its estimated width without fonts.
///
/// @param fonts the fonts; null to estimate
/// @param role which font
/// @return the measure
Measure measure_in(const Fonts* fonts, FontRole role) {
    if (fonts == nullptr)
        return estimated_width;
    return [fonts, role](std::string_view text) { return text_width(*fonts, role, text); };
}

/// The width a work dialog's line is fitted to. The smooth form measures the
/// font it draws, in points, so a line that fits is not cut at the estimate
/// of seven points a character. Without that form, the game font or the
/// estimate is used.
///
/// @param canvas the canvas the dialog is drawn on
/// @return the measure, in points
Measure work_line_measure(const Canvas& canvas) {
    if (canvas.smooth.fonts == nullptr || canvas.smooth.painter == nullptr)
        return measure_in(canvas.fonts, FontRole::small);
    const int32_t scale = canvas.smooth.scale_percent > 0 ? canvas.smooth.scale_percent : 100;
    const int32_t pixels = std::max(
        int32_t{1},
        static_cast<int32_t>(std::lround(
            static_cast<double>(smooth_text_points(FontRole::small)) * static_cast<double>(scale) /
            100.0
        ))
    );
    oa::platform::text_font::FontStack* fonts = canvas.smooth.fonts;
    return [fonts, pixels, scale](std::string_view text) {
        const int drawn =
            oa::ui::paint::text_width(*fonts, text, pixels, smooth_text_bold(FontRole::small));
        return static_cast<int32_t>((int64_t{drawn} * 100) / scale);
    };
}

/// Returns a text as a notice shows it.
///
/// @param look_up the caller's look-up; unset shows the text as given
/// @param text the text
/// @return the text shown
std::string shown(const LookUp& look_up, std::string_view text) {
    return look_up ? std::string(look_up(text)) : std::string(text);
}

/// Appends a control.
///
/// @param[in,out] list the display list
/// @param id its number
/// @param rect where it lies
/// @param name its automation name
/// @param text its caption, for automation
void add_button_control(
    DisplayList& list, ControlId id, const Rect& rect, std::string name, std::string text
) {
    Control control;
    control.id = id;
    control.rect = rect;
    control.name = std::move(name);
    control.kind = ControlKind::button;
    control.text = std::move(text);
    list.controls.push_back(std::move(control));
}

/// Tells whether a text may name a control, or start its name, to automation.
///
/// @param id the text
/// @return true for a word of a-z, 0-9 and hyphens
bool sound_id(std::string_view id) noexcept {
    return !id.empty() && std::all_of(id.begin(), id.end(), [](char character) {
        return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
               character == '-';
    });
}

/// Returns a control's name: a notice's or a question's word, then a word of its own.
///
/// @param word the notice's or the question's word
/// @param own the control's own word
/// @return <word>.<own>
std::string named(std::string_view word, std::string_view own) {
    return std::string(word) + "." + std::string(own);
}

/// Returns a notice's controls at a height, as a press tests them: OK, and
/// the open button when the notice has an open caption.
///
/// @param notice the notice
/// @param height the notice's height
/// @param ok OK's caption, for automation
/// @param open the open button's caption, for automation
/// @return the list, without items
DisplayList
notice_controls(const Notice& notice, int32_t height, std::string ok = {}, std::string open = {}) {
    const int32_t width = box_width(notice.size_class);
    const NoticeButtons buttons = notice_buttons(height, width);
    const std::string_view word = notice_word_of(notice);
    DisplayList list;
    add_button_control(list, notice_ok, buttons.ok, named(word, "ok"), std::move(ok));
    if (notice_opens(notice)) {
        add_button_control(list, notice_open, buttons.open, named(word, "open"), std::move(open));
        list.tab_order = {notice_open, notice_ok};
    } else {
        list.tab_order = {notice_ok};
    }
    return list;
}

/// Returns a question button's automation name.
///
/// @param word the question's word
/// @param button the button
/// @param index its place from the left, from 0
/// @return <word>.<id>, or <word>.button-<place from 1>
std::string
question_button_name(std::string_view word, const QuestionButton& button, std::size_t index) {
    if (sound_id(button.id))
        return named(word, button.id);
    return named(word, "button-" + std::to_string(index + 1));
}

/// Returns a question's controls at a height: its buttons, left to right.
///
/// @param question the question
/// @param height its height
/// @param fonts the fonts it is drawn in; null to estimate widths
/// @return the list, without items
DisplayList question_controls(const Question& question, int32_t height, const Fonts* fonts) {
    const std::vector<Rect> rects = question_button_rects(question, height, fonts);
    const std::string_view word = question_word_of(question);
    DisplayList list;
    for (std::size_t index = 0; index < rects.size(); ++index) {
        const auto id = static_cast<ControlId>(index);
        add_button_control(
            list,
            id,
            rects[index],
            question_button_name(word, question.buttons[index], index),
            question.buttons[index].caption
        );
        list.tab_order.push_back(id);
    }
    return list;
}

/// Appends a notice's or a question's text as a control automation reads
/// (notice_body): over the title and the lines that fit, taking no press.
///
/// @param[in,out] list the display list
/// @param word the notice's or the question's word
/// @param title the title's place
/// @param lines the lines that fit, placed
/// @param text the title, the paragraphs and the failure, as shown, one a line
void add_body(
    DisplayList& list,
    std::string_view word,
    const Rect& title,
    const std::vector<PlacedLine>& lines,
    std::string text,
    const std::vector<Rect>& more = {}
) {
    Rect over = title;
    const auto grow = [&over](const Rect& rect) {
        if (rect.width <= 0 || rect.height <= 0)
            return;
        const int32_t right = std::max(over.x + over.width, rect.x + rect.width);
        const int32_t bottom = std::max(over.y + over.height, rect.y + rect.height);
        over.x = std::min(over.x, rect.x);
        over.y = std::min(over.y, rect.y);
        over.width = right - over.x;
        over.height = bottom - over.y;
    };
    for (const PlacedLine& line : lines)
        grow(line.rect);
    for (const Rect& rect : more)
        grow(rect);
    Control body;
    body.id = notice_body;
    body.rect = over;
    body.enabled = false;
    body.name = named(word, "body");
    body.kind = ControlKind::area;
    body.focusable = false;
    body.text = std::move(text);
    list.controls.push_back(std::move(body));
}

/// Returns a notice's or a question's text as one line each: its title, its
/// paragraphs and its failure, when there is one.
///
/// @param title the title, as shown
/// @param paragraphs the paragraphs, as shown
/// @param failure the failure, as shown; empty for none
/// @return the lines, joined by line breaks
std::string
body_text(std::string title, const std::vector<std::string>& paragraphs, std::string_view failure) {
    std::string text = std::move(title);
    for (const std::string& paragraph : paragraphs)
        text += "\n" + paragraph;
    if (!failure.empty())
        text += "\n" + std::string(failure);
    return text;
}

/// Returns the header a notice or a question shows.
///
/// @param title the title, as shown
/// @param width the box's width
/// @return the header's look
HeaderLook header_of(std::string title, int32_t width) {
    HeaderLook header;
    header.width = width;
    header.title = std::move(title);
    header.title_width = title_rect(width).width;
    header.tracking = metrics.heading_tracking;
    return header;
}

/// Returns a question's title as its header shows it.
///
/// Measured as draw_header draws it: the regular font, the heading's
/// tracking, and the title's rectangle, after the font's stand-ins. A game
/// font draws the ellipsis as three stops, and the tracking counts each of
/// them, so the recorded line is wider than the ellipsis counted as one
/// character. A title that fits is unchanged. One that does not is cut
/// between words and ends with the ellipsis; a word that does not fit on its
/// own is cut by fit. With no fonts the title cannot be measured, so it is
/// unchanged.
///
/// @param title the title, as given
/// @param box_width the question's width
/// @param fonts the fonts; null leaves the title
/// @return the title the header draws
std::string title_for_header(std::string_view title, int32_t box_width, const Fonts* fonts) {
    if (fonts == nullptr || title.empty())
        return std::string(title);
    const int32_t room = title_rect(box_width).width;
    // draw_boxed_text stands the line in, then measures that. Counting the
    // ellipsis as one character leaves the three stops two columns over.
    const Measure measure = [fonts](std::string_view shown) {
        const std::string drawn = with_stand_ins(*fonts, FontRole::regular, shown);
        return tracked_width(*fonts, FontRole::regular, drawn, metrics.heading_tracking);
    };
    if (room <= 0 || measure(title) <= room)
        return std::string(title);
    std::string kept;
    std::string fitted;
    std::size_t at = 0;
    while (at < title.size()) {
        while (at < title.size() && title[at] == ' ')
            ++at;
        if (at >= title.size())
            break;
        const std::size_t start = at;
        while (at < title.size() && title[at] != ' ')
            ++at;
        const std::string word(title.substr(start, at - start));
        const std::string next = kept.empty() ? word : kept + " " + word;
        const std::string candidate = next + std::string(ellipsis);
        if (measure(candidate) > room)
            break;
        kept = next;
        fitted = candidate;
    }
    if (!fitted.empty())
        return fitted;
    return fit(title, room, measure);
}

/// Returns a button's look.
///
/// @param caption its caption, as shown
/// @param accent true for the accent look, false for the plain one
/// @param control its number
/// @param hovered the control under the pointer
/// @param pressed the control a held press is on
/// @return the look
ButtonLook button_look(
    std::string caption, bool accent, ControlId control, ControlId hovered, ControlId pressed
) {
    return {
        std::move(caption),
        accent ? ButtonStyle::accent : ButtonStyle::plain,
        hovered == control,
        pressed == control && hovered == control,
        true,
    };
}

/// Draws a line of text.
///
/// @param canvas where it is drawn
/// @param line the line
/// @param text the text, as shown
void draw_text_line(const Canvas& canvas, const PlacedLine& line, std::string_view text) {
    box_text(
        canvas,
        line.path ? FontRole::regular : FontRole::small,
        text,
        line.rect,
        Align::left,
        line.failure ? colour::lock : (line.path ? colour::hint : colour::text)
    );
}

/// Draws a button, and the focus ring round it when the keys mark it.
///
/// @param canvas where it is drawn
/// @param rect the button
/// @param look its look
/// @param marked true when the keys mark it
void draw_marked_button(
    const Canvas& canvas, const Rect& rect, const ButtonLook& look, bool marked
) {
    draw_button(canvas, rect, look);
    if (marked)
        draw_focus_ring(canvas, rect, true);
}

/// Appends a box's face and header: the window's face and the header with
/// its title.
///
/// @param[in,out] list the display list
/// @param height the box's height
/// @param width the box's width
/// @param title the title, as shown
void add_face_and_header(DisplayList& list, int32_t height, int32_t width, std::string title) {
    Item face;
    face.role = Role::fill;
    face.rect = {0, 0, width, height};
    face.colour = colour::panel;
    list.items.push_back(std::move(face));
    Item header;
    header.role = Role::header;
    header.rect = {metrics.edge, metrics.edge, inner_width_in(width), metrics.header_height};
    header.text = title;
    header.look = header_of(std::move(title), width);
    list.items.push_back(std::move(header));
}

/// Appends a line of text.
///
/// @param[in,out] list the display list
/// @param line the line
/// @param text the text, as shown
void add_text_line(DisplayList& list, const PlacedLine& line, std::string text) {
    Item item;
    item.role = Role::text;
    item.rect = line.rect;
    item.text = std::move(text);
    item.font = line.path ? FontRole::regular : FontRole::small;
    item.align = Align::left;
    item.colour = line.failure ? colour::lock : (line.path ? colour::hint : colour::text);
    list.items.push_back(std::move(item));
}

/// Appends the footer's rule and band.
///
/// @param[in,out] list the display list
/// @param footer_rule the row of the line over the footer
/// @param width the box's width
void add_footer(DisplayList& list, int32_t footer_rule, int32_t width) {
    Item item;
    item.role = Role::footer_band;
    item.rect = {metrics.edge, footer_rule, inner_width_in(width), 1 + metrics.footer_height};
    item.look = FooterBandLook{width, footer_rule};
    list.items.push_back(std::move(item));
}

/// Appends a button's item, and the focus ring's when the keys mark it. The
/// control is added apart, in the order a press tests it.
///
/// @param[in,out] list the display list
/// @param rect the button
/// @param look its look
/// @param id its number
/// @param marked true when the keys mark it
void add_button_item(
    DisplayList& list, const Rect& rect, const ButtonLook& look, ControlId id, bool marked
) {
    Item item;
    item.role = Role::button;
    item.rect = rect;
    item.control = id;
    item.text = look.caption;
    item.state.hovered = look.hovered;
    item.state.pressed = look.held;
    item.state.focused = marked;
    item.look = look;
    list.items.push_back(std::move(item));
    if (marked)
        add_focus_ring(list, rect, true);
}

/// Appends the window's edge.
///
/// @param[in,out] list the display list
/// @param height the box's height
/// @param width the box's width
void add_edge(DisplayList& list, int32_t height, int32_t width) {
    Item item;
    item.role = Role::bevel;
    item.rect = {0, 0, width, height};
    item.colour = colour::edge_light;
    list.items.push_back(std::move(item));
}

/// Returns what pressing a notice's button asks.
///
/// @param control the button
/// @return NoticeAction::open_folder for the open button, closed for OK
NoticeAction press(ControlId control) noexcept {
    return control == notice_open ? NoticeAction::open_folder : NoticeAction::closed;
}

/// Returns an answer.
///
/// @param question the question
/// @param button the button pressed
/// @return the answer; nothing for a button the question lacks
QuestionAnswer answer(const Question& question, int32_t button) {
    if (button < 0 || static_cast<std::size_t>(button) >= question.buttons.size())
        return {};
    return {QuestionAction::answered, button};
}

/// Returns a redraw, or nothing.
///
/// @param redraw whether the look changed
/// @return the answer
QuestionAnswer look_changed(bool redraw) {
    return {redraw ? QuestionAction::redraw : QuestionAction::none, -1};
}

/// Narrows a row of buttons until it and its gaps fit a room: a point at a
/// time off the widest, so the widest come to one width. None is narrowed
/// below button_width.
///
/// @param[in,out] widths the buttons' widths, left to right
/// @param room the row's room, in points
void narrow_row(std::vector<int32_t>& widths, int32_t room) {
    if (widths.empty())
        return;
    int32_t total = static_cast<int32_t>(widths.size() - 1) * metrics.button_gap;
    for (const int32_t width : widths)
        total += width;
    while (total > room) {
        const auto widest = std::max_element(widths.begin(), widths.end());
        if (*widest <= metrics.button_width)
            return;
        --*widest;
        --total;
    }
}

} // namespace

PlacedNotice place_notice(const Notice& notice, const Measure& regular, const Measure& small) {
    const int32_t width = box_width(notice.size_class);
    PlacedNotice placed{};
    placed.title = title_rect(width);
    PlacedText text = place_text(notice.paragraphs, notice.failure, regular, small, width);
    placed.height = height_for(text.bottom, notice.size_class);
    placed.footer_rule = footer_rule_of(placed.height);
    // A line the height cuts is left out.
    placed.lines = lines_that_fit(std::move(text.lines), placed.footer_rule);
    const NoticeButtons buttons = notice_buttons(placed.height, width);
    placed.ok_button = buttons.ok;
    if (notice_opens(notice))
        placed.open_button = buttons.open;
    return placed;
}

PlacedNotice place_notice(const Notice& notice, const Fonts* fonts) {
    return place_notice(
        notice, measure_in(fonts, FontRole::regular), measure_in(fonts, FontRole::small)
    );
}

std::vector<Rect>
question_button_rects(const Question& question, int32_t height, const Measure& small) {
    const int32_t top = button_top_of(height);
    const std::size_t count = std::min(question.buttons.size(), most_question_buttons);
    std::vector<int32_t> widths(count);
    for (std::size_t index = 0; index < count; ++index) {
        const std::string& caption = question.buttons[index].caption;
        const int32_t words = std::max(estimated_width(caption), small ? small(caption) : 0);
        widths[index] = std::max(metrics.button_width, words + metrics.prompt_button_padding);
    }
    const int32_t width = box_width(question.size_class);
    narrow_row(widths, width - 2 * metrics.padding);
    std::vector<Rect> rects(count);
    int32_t right = width - metrics.padding;
    for (std::size_t index = count; index-- > 0;) {
        rects[index] = {right - widths[index], top, widths[index], metrics.button_height};
        right -= widths[index] + metrics.button_gap;
    }
    return rects;
}

std::vector<Rect>
question_button_rects(const Question& question, int32_t height, const Fonts* fonts) {
    return question_button_rects(question, height, measure_in(fonts, FontRole::small));
}

/// Places one question item on the row it starts at.
///
/// @param item the item
/// @param row the badge's top
/// @param room the name's room, in points
/// @param small a text's width in the small font
/// @return the placed item
PlacedQuestionItem
place_question_item(const QuestionItem& item, int32_t row, int32_t room, const Measure& small) {
    constexpr int32_t kItemLine = 20;
    PlacedQuestionItem placed;
    placed.badge = {text_left, row, kItemLine, kItemLine};
    placed.look = item.badge;
    placed.text = {
        text_left + kItemLine + metrics.list_row_gap,
        row + (kItemLine - metrics.small_line) / 2,
        room,
        metrics.small_line,
    };
    const std::string combined = item.text + item.aside;
    placed.shown = fit(combined, room, small);
    placed.text_bytes = placed.shown.size();
    if (placed.shown.size() >= item.text.size() &&
        placed.shown.compare(0, item.text.size(), item.text) == 0)
        placed.text_bytes = item.text.size();
    return placed;
}

PlacedQuestion
place_question(const Question& question, const Measure& regular, const Measure& small) {
    const int32_t width = box_width(question.size_class);
    PlacedQuestion placed{};
    placed.title = title_rect(width);
    if (question.items.empty()) {
        PlacedText text = place_text(question.paragraphs, question.failure, regular, small, width);
        int32_t bottom = text.bottom;
        if (question.progress >= 0) {
            bottom += metrics.paragraph_gap;
            placed.bar = {text_left, bottom, line_width_in(width), metrics.progress_bar_height};
            bottom += metrics.progress_bar_height;
        }
        placed.height = height_for(bottom, question.size_class);
        placed.footer_rule = footer_rule_of(placed.height);
        const std::size_t all_lines = text.lines.size();
        placed.lines = lines_that_fit(std::move(text.lines), placed.footer_rule);
        const bool bar_cut = question.progress >= 0 && !fits_over(placed.bar, placed.footer_rule);
        placed.cut = placed.lines.size() != all_lines || bar_cut;
        // A bar the height cuts is left out.
        if (bar_cut)
            placed.bar = {};
        placed.buttons = question_button_rects(question, placed.height, small);
        return placed;
    }

    PlacedText text = place_text(question.paragraphs, std::string_view{}, regular, small, width);
    int32_t row = text.bottom;
    const int32_t line_width = line_width_in(width);
    constexpr int32_t kItemLine = 20;
    const int32_t room = std::max(line_width - kItemLine - metrics.list_row_gap, int32_t{0});
    const bool paragraphs = !text.lines.empty();
    if (paragraphs)
        row += metrics.paragraph_gap;
    // The design's prompts leave 4 points between item lines, so two badges never touch.
    constexpr int32_t kItemGap = 4;
    for (const QuestionItem& item : question.items) {
        if (!placed.items.empty())
            row += kItemGap;
        placed.items.push_back(place_question_item(item, row, room, small));
        row += kItemLine;
    }
    if (!question.failure.empty()) {
        if (paragraphs || !question.items.empty())
            row += metrics.paragraph_gap;
        for (const std::string& line : wrap(question.failure, line_width, small)) {
            text.lines.push_back(
                PlacedLine{line, false, true, {text_left, row, line_width, metrics.small_line}}
            );
            row += metrics.small_line;
        }
    }
    int32_t bottom = row;
    if (question.progress >= 0) {
        bottom += metrics.paragraph_gap;
        placed.bar = {text_left, bottom, line_width, metrics.progress_bar_height};
        bottom += metrics.progress_bar_height;
    }
    placed.height = height_for(bottom, question.size_class);
    placed.footer_rule = footer_rule_of(placed.height);
    const std::size_t all_lines = text.lines.size();
    const std::size_t all_items = placed.items.size();
    placed.lines = lines_that_fit(std::move(text.lines), placed.footer_rule);
    std::vector<PlacedQuestionItem> fitting;
    fitting.reserve(placed.items.size());
    for (PlacedQuestionItem& item : placed.items)
        if (fits_over(item.badge, placed.footer_rule))
            fitting.push_back(std::move(item));
    placed.items = std::move(fitting);
    const bool bar_cut = question.progress >= 0 && !fits_over(placed.bar, placed.footer_rule);
    placed.cut = placed.lines.size() != all_lines || placed.items.size() != all_items || bar_cut;
    if (bar_cut)
        placed.bar = {};
    placed.buttons = question_button_rects(question, placed.height, small);
    return placed;
}

PlacedQuestion place_question(const Question& question, const Fonts* fonts) {
    return place_question(
        question, measure_in(fonts, FontRole::regular), measure_in(fonts, FontRole::small)
    );
}

std::string_view notice_word_of(const Notice& notice) noexcept {
    return sound_id(notice.word) ? std::string_view(notice.word) : notice_word;
}

std::string_view question_word_of(const Question& question) noexcept {
    return sound_id(question.word) ? std::string_view(question.word) : question_word;
}

DisplayList notice_list(const Notice& notice, const Fonts* fonts, const LookUp& look_up) {
    const PlacedNotice placed = place_notice(notice, fonts);
    const int32_t width = box_width(notice.size_class);
    const std::string ok = shown(look_up, notice.ok_caption);
    const std::string open = shown(look_up, notice.open_caption);
    const std::string_view word = notice_word_of(notice);
    DisplayList list = notice_controls(notice, placed.height, ok, open);
    std::vector<std::string> paragraphs;
    for (const Paragraph& paragraph : notice.paragraphs)
        paragraphs.push_back(shown(look_up, paragraph.text));
    add_body(
        list,
        word,
        placed.title,
        placed.lines,
        body_text(
            shown(look_up, notice.title),
            paragraphs,
            notice.failure.empty() ? std::string() : shown(look_up, notice.failure)
        )
    );
    add_face_and_header(list, placed.height, width, shown(look_up, notice.title));
    for (const PlacedLine& line : placed.lines)
        add_text_line(list, line, shown(look_up, line.text));
    add_footer(list, placed.footer_rule, width);
    if (notice_opens(notice)) {
        add_button_item(
            list,
            placed.open_button,
            button_look(open, false, notice_open, notice.hovered, notice.pressed),
            notice_open,
            notice.marked == notice_open
        );
    }
    add_button_item(
        list,
        placed.ok_button,
        button_look(ok, true, notice_ok, notice.hovered, notice.pressed),
        notice_ok,
        notice.marked == notice_ok
    );
    add_edge(list, placed.height, width);
    return list;
}

/// Appends one placed question item: its badge, then the name and the aside.
///
/// @param[in,out] list the display list
/// @param item the item
/// @param fonts the fonts the name's width is measured in; null draws the aside at the name's start
void add_question_item(DisplayList& list, const PlacedQuestionItem& item, const Fonts* fonts) {
    add_badge(list, item.badge, item.look);
    const std::string_view shown = item.shown;
    const std::size_t name_bytes = std::min(item.text_bytes, shown.size());
    const std::string_view name = shown.substr(0, name_bytes);
    const std::string_view aside = shown.substr(name_bytes);
    const int32_t measured =
        fonts == nullptr || name.empty() ? 0 : text_width(*fonts, FontRole::small, name);
    const int32_t name_width = std::min(measured, std::max(item.text.width, int32_t{0}));
    if (!name.empty() && name_width > 0) {
        Item text;
        text.role = Role::text;
        text.rect = {item.text.x, item.text.y, name_width, item.text.height};
        text.text = std::string(name);
        text.font = FontRole::small;
        text.align = Align::left;
        text.colour = colour::text;
        list.items.push_back(std::move(text));
    }
    const int32_t aside_width = std::max(item.text.width - name_width, int32_t{0});
    if (!aside.empty() && aside_width > 0) {
        Item text;
        text.role = Role::text;
        text.rect = {item.text.x + name_width, item.text.y, aside_width, item.text.height};
        text.text = std::string(aside);
        text.font = FontRole::small;
        text.align = Align::left;
        text.colour = colour::hint;
        list.items.push_back(std::move(text));
    }
}

/// Returns a question's body text. With no items it is the title, the
/// paragraphs and the failure. With items, each item's name and aside is a
/// line after the paragraphs and before the failure.
///
/// @param question the question
/// @param title the title the header shows, already cut when it does not fit
/// @param paragraphs the paragraphs, as shown
/// @return the lines, joined by line breaks
std::string question_body(
    const Question& question, std::string title, const std::vector<std::string>& paragraphs
) {
    if (question.items.empty())
        return body_text(std::move(title), paragraphs, question.failure);
    std::string text = body_text(std::move(title), paragraphs, {});
    for (const QuestionItem& item : question.items) {
        text += '\n';
        text += item.text;
        text += item.aside;
    }
    if (!question.failure.empty()) {
        text += '\n';
        text += question.failure;
    }
    return text;
}

DisplayList question_list(const Question& question, const Fonts* fonts) {
    const PlacedQuestion placed = place_question(question, fonts);
    const int32_t width = box_width(question.size_class);
    const std::string title = title_for_header(question.title, width, fonts);
    DisplayList list = question_controls(question, placed.height, fonts);
    std::vector<std::string> paragraphs;
    for (const Paragraph& paragraph : question.paragraphs)
        paragraphs.push_back(paragraph.text);
    std::vector<Rect> item_rects;
    for (const PlacedQuestionItem& item : placed.items) {
        item_rects.push_back(item.badge);
        item_rects.push_back(item.text);
    }
    add_body(
        list,
        question_word_of(question),
        placed.title,
        placed.lines,
        question_body(question, title, paragraphs),
        item_rects
    );
    add_face_and_header(list, placed.height, width, title);
    if (question.items.empty()) {
        for (const PlacedLine& line : placed.lines)
            add_text_line(list, line, line.text);
    } else {
        for (const PlacedLine& line : placed.lines)
            if (!line.failure)
                add_text_line(list, line, line.text);
        for (const PlacedQuestionItem& item : placed.items)
            add_question_item(list, item, fonts);
        for (const PlacedLine& line : placed.lines)
            if (line.failure)
                add_text_line(list, line, line.text);
    }
    add_footer(list, placed.footer_rule, width);
    if (placed.bar.width > 0)
        add_progress(list, placed.bar, question.progress, question_progress_whole);
    for (std::size_t index = 0; index < placed.buttons.size(); ++index) {
        const auto control = static_cast<ControlId>(index);
        const QuestionButton& button = question.buttons[index];
        add_button_item(
            list,
            placed.buttons[index],
            button_look(button.caption, button.accent, control, question.hovered, question.pressed),
            control,
            question.marked == control
        );
    }
    add_edge(list, placed.height, width);
    return list;
}

NoticeAction notice_pointer_move(Notice& notice, Point at, int32_t height) {
    // A finger's held press moves as it was moved to the button it took.
    if (notice.pressed != no_control) {
        at.x += notice.finger_shift_x;
        at.y += notice.finger_shift_y;
    }
    const ControlId hovered = hit(notice_controls(notice, height), at);
    if (hovered == notice.hovered)
        return NoticeAction::none;
    notice.hovered = hovered;
    return NoticeAction::redraw;
}

NoticeAction notice_pointer_down(Notice& notice, Point at, int32_t height) {
    notice.finger_shift_x = 0;
    notice.finger_shift_y = 0;
    notice.hovered = hit(notice_controls(notice, height), at);
    if (notice.hovered == no_control)
        return NoticeAction::none;
    notice.pressed = notice.hovered;
    return NoticeAction::redraw;
}

NoticeAction notice_finger_down(Notice& notice, Point finger, int32_t height, int32_t reach_px) {
    // OK, then the open button: the nearer within reach, at its point nearest
    // the finger.
    const Reached landed = reach(notice_controls(notice, height), finger, reach_px);
    const NoticeAction action = notice_pointer_down(notice, landed.at, height);
    if (action == NoticeAction::redraw) {
        notice.finger_shift_x = landed.at.x - finger.x;
        notice.finger_shift_y = landed.at.y - finger.y;
    }
    return action;
}

NoticeAction notice_pointer_up(Notice& notice, Point at, int32_t height) {
    // A finger's release lands as its press was moved; the next press
    // starts afresh.
    if (notice.pressed != no_control) {
        at.x += notice.finger_shift_x;
        at.y += notice.finger_shift_y;
    }
    notice.finger_shift_x = 0;
    notice.finger_shift_y = 0;
    notice.hovered = hit(notice_controls(notice, height), at);
    const ControlId held = notice.pressed;
    notice.pressed = no_control;
    if (held == no_control)
        return NoticeAction::none;
    if (held != notice.hovered)
        return NoticeAction::redraw;
    return press(held);
}

NoticeAction notice_key(Notice& notice, Key key) {
    // No second button: every key that would move the mark stays on OK.
    if (!notice_opens(notice)) {
        notice.marked = notice_ok;
        switch (key) {
        case Key::enter:
        case Key::escape:
        case Key::space:
            return NoticeAction::closed;
        default:
            return NoticeAction::none;
        }
    }
    switch (key) {
    case Key::enter:
    case Key::escape:
        return NoticeAction::closed;
    case Key::space:
        return press(notice.marked);
    case Key::left:
    case Key::right:
    case Key::up:
    case Key::down:
    case Key::tab:
    case Key::back_tab:
        notice.marked = notice.marked == notice_ok ? notice_open : notice_ok;
        return NoticeAction::redraw;
    default:
        return NoticeAction::none;
    }
}

QuestionAnswer
question_pointer_move(Question& question, Point at, int32_t height, const Fonts* fonts) {
    if (question.pressed != no_control) {
        at.x += question.finger_shift_x;
        at.y += question.finger_shift_y;
    }
    const ControlId hovered = hit(question_controls(question, height, fonts), at);
    if (hovered == question.hovered)
        return {};
    question.hovered = hovered;
    return look_changed(true);
}

QuestionAnswer
question_pointer_down(Question& question, Point at, int32_t height, const Fonts* fonts) {
    question.finger_shift_x = 0;
    question.finger_shift_y = 0;
    question.hovered = hit(question_controls(question, height, fonts), at);
    if (question.hovered == no_control)
        return {};
    question.pressed = question.hovered;
    return look_changed(true);
}

QuestionAnswer question_finger_down(
    Question& question, Point finger, int32_t height, int32_t reach_px, const Fonts* fonts
) {
    // The nearest button within reach, left to right, at its point nearest
    // the finger.
    const Reached landed = reach(question_controls(question, height, fonts), finger, reach_px);
    const QuestionAnswer pressed = question_pointer_down(question, landed.at, height, fonts);
    if (pressed.action == QuestionAction::redraw) {
        question.finger_shift_x = landed.at.x - finger.x;
        question.finger_shift_y = landed.at.y - finger.y;
    }
    return pressed;
}

QuestionAnswer
question_pointer_up(Question& question, Point at, int32_t height, const Fonts* fonts) {
    if (question.pressed != no_control) {
        at.x += question.finger_shift_x;
        at.y += question.finger_shift_y;
    }
    question.finger_shift_x = 0;
    question.finger_shift_y = 0;
    question.hovered = hit(question_controls(question, height, fonts), at);
    const ControlId held = question.pressed;
    question.pressed = no_control;
    if (held == no_control)
        return {};
    if (held != question.hovered)
        return look_changed(true);
    return answer(question, held);
}

QuestionAnswer question_key(Question& question, Key key) {
    const auto count =
        static_cast<int32_t>(std::min(question.buttons.size(), most_question_buttons));
    if (count == 0)
        return {};
    switch (key) {
    case Key::enter:
    case Key::space:
        return answer(question, question.marked);
    case Key::escape:
    case Key::no:
        return answer(question, question.cancel_button);
    case Key::yes:
        return answer(question, question.primary_button);
    case Key::left:
    case Key::up:
    case Key::back_tab:
        question.marked = (question.marked + count - 1) % count;
        return look_changed(count > 1);
    case Key::right:
    case Key::down:
    case Key::tab:
        question.marked = (question.marked + 1) % count;
        return look_changed(count > 1);
    default:
        return {};
    }
}

void draw_notice(const Canvas& canvas, const Notice& notice, const LookUp& look_up) {
    const PlacedNotice placed = place_notice(notice, canvas.fonts);
    const int32_t width = box_width(notice.size_class);
    const Rect whole{0, 0, width, placed.height};
    draw_window_face(canvas, whole);
    draw_header(canvas, header_of(shown(look_up, notice.title), width));
    // The text: paths in the regular font, the rest in the small one, the
    // failure in amber.
    for (const PlacedLine& line : placed.lines)
        draw_text_line(canvas, line, shown(look_up, line.text));
    draw_footer_band(canvas, width, placed.footer_rule);
    // The footer: the open button plain and OK in the accent, the marked one
    // ringed.
    if (notice_opens(notice)) {
        draw_marked_button(
            canvas,
            placed.open_button,
            button_look(
                shown(look_up, notice.open_caption),
                false,
                notice_open,
                notice.hovered,
                notice.pressed
            ),
            notice.marked == notice_open
        );
    }
    draw_marked_button(
        canvas,
        placed.ok_button,
        button_look(
            shown(look_up, notice.ok_caption), true, notice_ok, notice.hovered, notice.pressed
        ),
        notice.marked == notice_ok
    );
    draw_window_edge(canvas, whole);
}

/// Draws one placed question item: the badge, the name and the aside.
///
/// @param canvas where it is drawn
/// @param item the item
void draw_question_item(const Canvas& canvas, const PlacedQuestionItem& item) {
    draw_badge(canvas, item.badge, item.look);
    const std::string_view shown = item.shown;
    const std::size_t name_bytes = std::min(item.text_bytes, shown.size());
    const std::string_view name = shown.substr(0, name_bytes);
    const std::string_view aside = shown.substr(name_bytes);
    const int32_t measured = name.empty() ? 0 : measure_text(canvas, FontRole::small, name);
    const int32_t name_width = std::min(measured, std::max(item.text.width, int32_t{0}));
    if (!name.empty())
        box_text(
            canvas,
            FontRole::small,
            name,
            {item.text.x, item.text.y, name_width, item.text.height},
            Align::left,
            colour::text
        );
    const int32_t aside_width = std::max(item.text.width - name_width, int32_t{0});
    if (!aside.empty() && aside_width > 0)
        box_text(
            canvas,
            FontRole::small,
            aside,
            {item.text.x + name_width, item.text.y, aside_width, item.text.height},
            Align::left,
            colour::hint
        );
}

void draw_question(const Canvas& canvas, const Question& question) {
    const PlacedQuestion placed = place_question(question, canvas.fonts);
    const int32_t width = box_width(question.size_class);
    const Rect whole{0, 0, width, placed.height};
    draw_window_face(canvas, whole);
    draw_header(canvas, header_of(title_for_header(question.title, width, canvas.fonts), width));
    if (question.items.empty()) {
        for (const PlacedLine& line : placed.lines)
            draw_text_line(canvas, line, line.text);
    } else {
        for (const PlacedLine& line : placed.lines)
            if (!line.failure)
                draw_text_line(canvas, line, line.text);
        for (const PlacedQuestionItem& item : placed.items)
            draw_question_item(canvas, item);
        for (const PlacedLine& line : placed.lines)
            if (line.failure)
                draw_text_line(canvas, line, line.text);
    }
    draw_footer_band(canvas, width, placed.footer_rule);
    if (placed.bar.width > 0)
        draw_progress(canvas, placed.bar, question.progress, question_progress_whole);
    for (std::size_t index = 0; index < placed.buttons.size(); ++index) {
        const auto control = static_cast<ControlId>(index);
        const QuestionButton& button = question.buttons[index];
        draw_marked_button(
            canvas,
            placed.buttons[index],
            button_look(button.caption, button.accent, control, question.hovered, question.pressed),
            question.marked == control
        );
    }
    draw_window_edge(canvas, whole);
}

void draw_progress(
    const Canvas& canvas, const Rect& bar, int32_t done, int32_t whole, bool paused
) {
    if (!can_draw(canvas) || bar.width <= 0 || bar.height <= 0)
        return;
    const int32_t radius = std::min(bar.width, bar.height) / 2;
    fill_round_rect(canvas, bar, radius, colour::list_selected);
    if (whole <= 0)
        return;
    const auto filled =
        static_cast<int32_t>(int64_t{bar.width} * std::clamp(done, int32_t{0}, whole) / whole);
    if (filled > 0)
        fill_round_rect(
            canvas,
            {bar.x, bar.y, filled, bar.height},
            radius,
            paused ? colour::lock : colour::accent
        );
}

void add_progress(DisplayList& list, const Rect& bar, int32_t done, int32_t whole, bool paused) {
    Item item;
    item.role = Role::progress;
    item.rect = bar;
    item.text = std::to_string(done);
    item.look = ProgressLook{done, whole, paused};
    list.items.push_back(std::move(item));
}

PlacedWorkDialog place_work_dialog(const WorkDialog& dialog, const Measure& measure) {
    const int32_t width = box_width(dialog.size_class);
    const Metrics& sized = metrics_of(dialog.size_class);
    const bool spinner = dialog.progress < 0;
    const int32_t indicator = spinner ? work_spinner_side : metrics.progress_bar_height;
    const int32_t indicator_y = metrics.text_top + metrics.small_line + metrics.paragraph_gap;
    const int32_t content_bottom = indicator_y + indicator;
    const int32_t natural = content_bottom + metrics.text_bottom_gap + metrics.edge;
    PlacedWorkDialog placed;
    placed.height = std::min(natural, sized.greatest_notice_height);
    placed.title = title_rect(width);
    const int32_t line_width = line_width_in(width);
    placed.line = PlacedLine{
        fit(dialog.line, line_width, measure),
        false,
        false,
        {text_left, metrics.text_top, line_width, metrics.small_line}
    };
    if (spinner) {
        placed.spinner = {
            text_left + (line_width - work_spinner_side) / 2,
            indicator_y,
            work_spinner_side,
            work_spinner_side
        };
    } else {
        placed.bar = {text_left, indicator_y, line_width, metrics.progress_bar_height};
    }
    return placed;
}

PlacedWorkDialog place_work_dialog(const WorkDialog& dialog, const Fonts* fonts) {
    return place_work_dialog(dialog, measure_in(fonts, FontRole::small));
}

DisplayList work_dialog_list(const WorkDialog& dialog, const Fonts* fonts) {
    const PlacedWorkDialog placed = place_work_dialog(dialog, fonts);
    const int32_t width = box_width(dialog.size_class);
    DisplayList list;
    add_body(
        list,
        work_dialog_word,
        placed.title,
        {placed.line},
        body_text(dialog.title, {placed.line.text}, {})
    );
    add_face_and_header(list, placed.height, width, dialog.title);
    add_text_line(list, placed.line, placed.line.text);
    if (placed.bar.width > 0)
        add_progress(list, placed.bar, dialog.progress, question_progress_whole);
    add_edge(list, placed.height, width);
    return list;
}

void draw_work_dialog(const Canvas& canvas, const WorkDialog& dialog) {
    const PlacedWorkDialog placed = place_work_dialog(dialog, work_line_measure(canvas));
    const int32_t width = box_width(dialog.size_class);
    const Rect whole{0, 0, width, placed.height};
    draw_window_face(canvas, whole);
    draw_header(canvas, header_of(dialog.title, width));
    draw_text_line(canvas, placed.line, placed.line.text);
    if (placed.bar.width > 0)
        draw_progress(canvas, placed.bar, dialog.progress, question_progress_whole);
    else if (placed.spinner.width > 0)
        draw_spinner(canvas, placed.spinner, dialog.turn);
    draw_window_edge(canvas, whole);
}

} // namespace oa::ui::kit
