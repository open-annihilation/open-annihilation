// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Points, scale, size classes, arrangements, scroll areas and the display list,
// and the parts of a screen drawn in the modern fonts.

#include "oa/ui/kit/layout.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdint.h>
#include <string>
#include <utility>
#include <vector>

namespace oa::ui::kit {

namespace {

/// Returns floor(value * numerator / denominator). A denominator of 0 or less counts as 1.
///
/// @param value the value
/// @param numerator the multiplier
/// @param denominator the divisor
/// @return the rounded-down product
[[nodiscard]] int32_t
floor_mul_div(int32_t value, int32_t numerator, int32_t denominator) noexcept {
    const int32_t divisor = denominator > 0 ? denominator : 1;
    const int64_t product = static_cast<int64_t>(value) * numerator;
    int64_t quotient = product / divisor;
    if (product % divisor < 0)
        --quotient;
    return static_cast<int32_t>(quotient);
}

/// Returns value * numerator / denominator rounded to the nearest whole
/// number, a half away from zero. A denominator of 0 or less counts as 1.
///
/// @param value the value
/// @param numerator the multiplier
/// @param denominator the divisor
/// @return the rounded product
[[nodiscard]] int32_t
nearest_mul_div(int32_t value, int32_t numerator, int32_t denominator) noexcept {
    const int64_t divisor = denominator > 0 ? denominator : 1;
    const int64_t product = static_cast<int64_t>(value) * numerator;
    const int64_t magnitude = (product < 0 ? -product : product) * 2 + divisor;
    const int64_t rounded = magnitude / (2 * divisor);
    return static_cast<int32_t>(product < 0 ? -rounded : rounded);
}

/// Returns the percentage a scale divides by. 0 or less counts as 1.
///
/// @param scale_percent the scale, as a percentage
/// @return the percentage, at least 1
[[nodiscard]] int32_t percent_of(int32_t scale_percent) noexcept {
    return scale_percent > 0 ? scale_percent : 1;
}

/// Returns how many columns fit in a width at a least cell width.
///
/// @param width the row's width, in points
/// @param least the least cell width, in points
/// @param gap the points between cells
/// @param count the cells to place
/// @return the columns, at least one when count is not 0
[[nodiscard]] int32_t
columns_across(int32_t width, int32_t least, int32_t gap, std::size_t count) noexcept {
    if (count == 0)
        return 0;
    int32_t columns = 1;
    if (least > 0) {
        const int64_t stride = static_cast<int64_t>(least) + gap;
        if (stride > 0 && width >= least) {
            const int64_t fit = (static_cast<int64_t>(width) + gap) / stride;
            if (fit > columns)
                columns = static_cast<int32_t>(fit);
        }
    }
    if (static_cast<std::size_t>(columns) > count)
        columns = static_cast<int32_t>(count);
    return columns;
}

/// Shares a length across parts. The first extra parts are one longer.
///
/// @param total the length, in points
/// @param parts how many parts
/// @param[out] base each part's length before the spare
/// @param[out] extra how many parts take one more point
void share(int32_t total, int32_t parts, int32_t& base, int32_t& extra) noexcept {
    if (parts <= 0) {
        base = 0;
        extra = 0;
        return;
    }
    int64_t quotient = static_cast<int64_t>(total) / parts;
    int64_t remainder = static_cast<int64_t>(total) % parts;
    if (remainder < 0) {
        --quotient;
        remainder += parts;
    }
    base = static_cast<int32_t>(quotient);
    extra = static_cast<int32_t>(remainder);
}

/// Returns the second part of a split, empty when it would have no room.
///
/// @param origin the area's origin on this axis
/// @param extent the area's extent on this axis
/// @param first the first part's extent
/// @param gap the points between the parts
/// @param[out] start the second part's origin
/// @return the second part's extent, 0 when none is left
[[nodiscard]] int32_t
rest_of(int32_t origin, int32_t extent, int32_t first, int32_t gap, int32_t& start) noexcept {
    start = origin + first + gap;
    const int32_t room = origin + extent - start;
    return room > 0 ? room : 0;
}

} // namespace

int32_t auto_scale(int32_t canvas_height, float density, bool offer_fractional) noexcept {
    const double ratio =
        static_cast<double>(canvas_height) / static_cast<double>(auto_scale_height);
    // 1.25 and 1.75 times 720: the ties between 1x and 150%, and 150% and 2x.
    // A tie takes the larger, so 150% runs from 900 lines up to 1260.
    constexpr int32_t fraction_from = auto_scale_height * 5 / 4;
    constexpr int32_t two_from = auto_scale_height * 7 / 4;
    const long density_step = std::max(long{1}, std::lround(static_cast<double>(density)));
    int32_t percent = 100;
    if (offer_fractional && canvas_height < two_from)
        percent = canvas_height < fraction_from ? 100 : fractional_scale_percent;
    else {
        const long step = std::max(long{1}, std::lround(ratio));
        percent = static_cast<int32_t>(step) * 100;
    }
    const int32_t floor_percent = static_cast<int32_t>(density_step) * 100;
    if (percent < floor_percent)
        percent = floor_percent;
    return percent;
}

SizeClass size_class_of(int32_t width_points, int32_t height_points) noexcept {
    if (width_points >= large_from.x && height_points >= large_from.y)
        return SizeClass::large;
    if (width_points >= regular_from.x && height_points >= regular_from.y)
        return SizeClass::regular;
    return SizeClass::compact;
}

SizeClass
size_class_within(SizeClass chosen, int32_t width_points, int32_t height_points) noexcept {
    const auto fits = [&](SizeClass size_class) {
        const Metrics& sized = metrics_of(size_class);
        return width_points >= sized.dialog_width && height_points >= sized.dialog_height;
    };
    if (chosen == SizeClass::large && fits(SizeClass::large))
        return SizeClass::large;
    if (chosen != SizeClass::compact && fits(SizeClass::regular))
        return SizeClass::regular;
    return SizeClass::compact;
}

Frame frame_of(const Viewport& viewport) noexcept {
    const int32_t percent = percent_of(viewport.scale_percent);
    const int32_t inner_width = viewport.width - viewport.safe.left - viewport.safe.right;
    const int32_t inner_height = viewport.height - viewport.safe.top - viewport.safe.bottom;
    Frame frame;
    frame.area = {
        floor_mul_div(viewport.safe.left, 100, percent),
        floor_mul_div(viewport.safe.top, 100, percent),
        floor_mul_div(inner_width, 100, percent),
        floor_mul_div(inner_height, 100, percent),
    };
    frame.size_class = size_class_of(frame.area.width, frame.area.height);
    frame.scale_percent = viewport.scale_percent;
    return frame;
}

Viewport layer_viewport(const Viewport& canvas, int32_t chosen_percent, SizeClass wanted) noexcept {
    const int32_t room_width = canvas.width - canvas.safe.left - canvas.safe.right;
    const int32_t room_height = canvas.height - canvas.safe.top - canvas.safe.bottom;
    Viewport viewport = canvas;
    // 0 or less is Auto on whole steps, except the one value that offers 150%.
    const bool offer_fractional = chosen_percent == auto_with_fraction_percent;
    int32_t percent = chosen_percent;
    if (chosen_percent <= 0) {
        percent = auto_scale(canvas.height, canvas.density, offer_fractional);
        // The scale serves the class: one step down where that alone gives
        // the room the wanted class, never below the density floor or 1x.
        const auto class_at = [&](int32_t scale_percent) {
            return size_class_of(
                floor_mul_div(room_width, 100, scale_percent),
                floor_mul_div(room_height, 100, scale_percent)
            );
        };
        int32_t below = percent - 100;
        if (offer_fractional && percent == 200)
            below = fractional_scale_percent;
        else if (percent == fractional_scale_percent)
            below = 100;
        const long density_step =
            std::max(long{1}, std::lround(static_cast<double>(canvas.density)));
        const int32_t floor_percent = static_cast<int32_t>(density_step) * 100;
        if (below >= 100 && below >= floor_percent && class_at(percent) < wanted &&
            class_at(below) >= wanted)
            percent = below;
    }
    if (percent == fractional_scale_percent) {
        // One and a half where Compact's dialog fits the room at it, else 1x.
        const bool fits_fraction =
            int64_t{room_width} * 100 >=
                int64_t{compact_metrics.dialog_width} * fractional_scale_percent &&
            int64_t{room_height} * 100 >=
                int64_t{compact_metrics.dialog_height} * fractional_scale_percent;
        viewport.scale_percent = fits_fraction ? fractional_scale_percent : 100;
        return viewport;
    }
    const int32_t chosen = percent > 0 ? percent / 100 : 1;
    // The largest whole step at which Compact's dialog fits the room.
    const int32_t fits = std::min(
        room_width / compact_metrics.dialog_width, room_height / compact_metrics.dialog_height
    );
    viewport.scale_percent = std::max(int32_t{1}, std::min(chosen, fits)) * 100;
    return viewport;
}

Rect to_canvas(const Rect& points, int32_t scale_percent) noexcept {
    const int32_t percent = percent_of(scale_percent);
    const int32_t left = nearest_mul_div(points.x, percent, 100);
    const int32_t top = nearest_mul_div(points.y, percent, 100);
    const int32_t right = nearest_mul_div(points.x + points.width, percent, 100);
    const int32_t bottom = nearest_mul_div(points.y + points.height, percent, 100);
    return {left, top, right - left, bottom - top};
}

Point to_points(Point canvas, int32_t scale_percent) noexcept {
    const int32_t percent = percent_of(scale_percent);
    return {floor_mul_div(canvas.x, 100, percent), floor_mul_div(canvas.y, 100, percent)};
}

std::vector<Rect> column(const Rect& area, std::span<const int32_t> heights, int32_t gap) {
    std::vector<Rect> placed;
    placed.reserve(heights.size());
    int32_t y = area.y;
    for (std::size_t index = 0; index < heights.size(); ++index) {
        placed.push_back(Rect{area.x, y, area.width, heights[index]});
        y += heights[index];
        if (index + 1 < heights.size())
            y += gap;
    }
    return placed;
}

std::vector<Rect> row(const Rect& area, std::span<const int32_t> widths, int32_t gap, Align align) {
    int64_t total = 0;
    for (const int32_t width : widths)
        total += width;
    if (widths.size() > 1)
        total += static_cast<int64_t>(gap) * static_cast<int64_t>(widths.size() - 1);
    int32_t x = area.x;
    const int64_t spare = static_cast<int64_t>(area.width) - total;
    if (align == Align::right)
        x += static_cast<int32_t>(spare);
    else if (align == Align::centre)
        x += static_cast<int32_t>(spare / 2);
    std::vector<Rect> placed;
    placed.reserve(widths.size());
    for (std::size_t index = 0; index < widths.size(); ++index) {
        placed.push_back(Rect{x, area.y, widths[index], area.height});
        x += widths[index];
        if (index + 1 < widths.size())
            x += gap;
    }
    return placed;
}

std::vector<Rect> grid(
    const Rect& area, int32_t least_cell_width, int32_t cell_height, int32_t gap, std::size_t count
) {
    std::vector<Rect> cells;
    const int32_t columns = columns_across(area.width, least_cell_width, gap, count);
    if (columns <= 0)
        return cells;
    int32_t base = 0;
    int32_t extra = 0;
    share(area.width - gap * (columns - 1), columns, base, extra);
    cells.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const int32_t column_index =
            static_cast<int32_t>(index % static_cast<std::size_t>(columns));
        const int32_t row_index = static_cast<int32_t>(index / static_cast<std::size_t>(columns));
        int32_t x = area.x;
        for (int32_t column = 0; column < column_index; ++column)
            x += base + (column < extra ? 1 : 0) + gap;
        const int32_t width = base + (column_index < extra ? 1 : 0);
        const int32_t y = area.y + row_index * (cell_height + gap);
        cells.push_back(Rect{x, y, width, cell_height});
    }
    return cells;
}

Split split_across(const Rect& area, int32_t first_width, int32_t gap) {
    int32_t x = 0;
    const int32_t width = rest_of(area.x, area.width, first_width, gap, x);
    return Split{
        Rect{area.x, area.y, first_width, area.height}, Rect{x, area.y, width, area.height}
    };
}

Split split_down(const Rect& area, int32_t first_height, int32_t gap) {
    int32_t y = 0;
    const int32_t height = rest_of(area.y, area.height, first_height, gap, y);
    return Split{
        Rect{area.x, area.y, area.width, first_height}, Rect{area.x, y, area.width, height}
    };
}

int32_t ScrollArea::limit_for(int32_t content_height, int32_t view_height) noexcept {
    return std::max(content_height - view_height, int32_t{0});
}

int32_t ScrollArea::offset_showing(int32_t content_top, int32_t height, bool last) const noexcept {
    const int32_t highest = content_top - view.y;
    const int32_t lowest = last ? limit : content_top + height - (view.y + view.height - 1);
    const int32_t scroll = std::min(std::max(offset, lowest), highest);
    if (limit < 0)
        return 0;
    return std::clamp(scroll, int32_t{0}, limit);
}

Rect ScrollArea::thumb(int32_t least_height) const noexcept {
    const Rect inside{well.x + 1, well.y + 1, well.width - 2, well.height - 2};
    int32_t height = inside.height;
    if (content_height > view.height && content_height != 0) {
        const int32_t share_height = static_cast<int32_t>(
            static_cast<int64_t>(inside.height) * view.height / content_height
        );
        height = std::max(least_height, share_height);
    }
    const int32_t travel = inside.height - height;
    int32_t top = inside.y;
    if (limit > 0) {
        const int32_t scroll = std::clamp(offset, int32_t{0}, limit);
        top += static_cast<int32_t>((static_cast<int64_t>(travel) * scroll + limit / 2) / limit);
    }
    return {inside.x, top, inside.width, height};
}

int32_t ScrollArea::offset_at(int32_t thumb_top, int32_t least_height) const noexcept {
    ScrollArea resting = *this;
    resting.offset = 0;
    const Rect at_rest = resting.thumb(least_height);
    const int32_t travel = well.height - 2 - at_rest.height;
    if (travel <= 0 || limit <= 0)
        return 0;
    const int32_t along = std::clamp(thumb_top - at_rest.y, int32_t{0}, travel);
    return static_cast<int32_t>((static_cast<int64_t>(limit) * along + travel / 2) / travel);
}

Rect ScrollArea::placed(const Rect& content) const noexcept {
    if (content.width <= 0 || content.height <= 0)
        return content;
    return {content.x, content.y - offset, content.width, content.height};
}

ControlId hit(const DisplayList& list, Point point) noexcept {
    return hit(list, point, DisabledControl::passes);
}

ControlId hit(const DisplayList& list, Point point, DisabledControl disabled) noexcept {
    for (const Control& control : list.controls) {
        const bool clipped = control.clip.width > 0 && control.clip.height > 0;
        const Rect reach = clipped ? intersect(control.rect, control.clip) : control.rect;
        if (!contains(reach, point))
            continue;
        if (control.enabled)
            return control.id;
        if (disabled == DisabledControl::blocks)
            return no_control;
    }
    return no_control;
}

const Control* control_of(const DisplayList& list, ControlId id) noexcept {
    for (const Control& control : list.controls)
        if (control.id == id)
            return &control;
    return nullptr;
}

int32_t canvas_pixels(const Typesetter& type, float points) noexcept {
    return static_cast<int32_t>(std::lround(points * type.px_per_point));
}

int32_t canvas_font(const Typesetter& type, float points) noexcept {
    return std::max(int32_t{1}, canvas_pixels(type, points));
}

int32_t text_part(
    std::vector<Item>& parts,
    const Typesetter& type,
    Role role,
    Point at,
    int32_t room,
    std::string_view words,
    const Lettering& look,
    TextFit sits,
    std::size_t most_lines,
    WordBreak break_word
) {
    if (words.empty() || room <= 0)
        return 0;
    const int32_t size = canvas_font(type, look.points);
    const Measure measure = [&type, size, &look](std::string_view shown) {
        return measured_width(type.measure, shown, size, look.bold);
    };
    WrapRules rules;
    rules.newlines = true;
    // A folder's path keeps to its separators, as break_word gives them.
    rules.wide_scripts = break_word == nullptr;
    rules.most_lines = most_lines;
    if (break_word != nullptr)
        rules.break_word = [break_word, room, &measure](std::string_view word) {
            return break_word(word, room, measure);
        };
    rules.shorten_last = [&measure, room](std::string_view joined) {
        return fit(std::string(joined) + std::string(ellipsis), room, measure);
    };
    std::vector<std::string> lines = wrap(words, room, measure, rules);
    lines.erase(
        std::remove_if(
            lines.begin(), lines.end(), [](const std::string& line) { return line.empty(); }
        ),
        lines.end()
    );
    if (lines.empty())
        return 0;
    int32_t widest = 0;
    for (const std::string& line : lines)
        widest = std::max(widest, measure(line));
    const int32_t width = sits == TextFit::column ? room : std::min(room, widest);
    Item part{};
    part.role = role;
    part.rect = {
        sits == TextFit::tight_right ? at.x + room - width : at.x,
        at.y,
        width,
        measured_line(type.measure, size, look.bold) * static_cast<int32_t>(lines.size())
    };
    part.colour = look.colour;
    part.lines = std::move(lines);
    part.pixel_size = size;
    part.bold = look.bold;
    part.style = look.style;
    parts.push_back(std::move(part));
    return parts.back().rect.height;
}

int32_t line_part(
    std::vector<Item>& parts,
    const Typesetter& type,
    int32_t x,
    int32_t middle,
    int32_t room,
    std::string_view words,
    const Lettering& look,
    TextFit sits,
    Role role
) {
    if (words.empty() || room <= 0)
        return 0;
    const int32_t size = canvas_font(type, look.points);
    const Measure measure = [&type, size, &look](std::string_view shown) {
        return measured_width(type.measure, shown, size, look.bold);
    };
    std::string line = fit(words, room, measure);
    if (line.empty())
        return 0;
    const int32_t line_height = measured_line(type.measure, size, look.bold);
    const int32_t width = std::min(room, measure(line));
    Item part{};
    part.role = role;
    part.rect = {
        sits == TextFit::tight_right ? x - width : x, middle - line_height / 2, width, line_height
    };
    part.colour = look.colour;
    part.lines = {std::move(line)};
    part.pixel_size = size;
    part.bold = look.bold;
    part.style = look.style;
    parts.push_back(std::move(part));
    return width;
}

namespace {

/// Returns the room a button's mark takes before its label: the mark's side and the gap
/// after it.
///
/// @param type the scale
/// @param button the button
/// @param label_size the label's pixel size
/// @return the room, in canvas pixels; 0 for a button with no mark
[[nodiscard]] int32_t
mark_room(const Typesetter& type, const ButtonSpec& button, int32_t label_size) noexcept {
    if (button.glyph == Glyph::none)
        return 0;
    const int32_t side =
        button.glyph_points > 0.0F ? canvas_pixels(type, button.glyph_points) : label_size;
    return side +
           static_cast<int32_t>(std::lround(static_cast<float>(label_size) * button_mark_gap_em));
}

} // namespace

int32_t button_span(
    const Typesetter& type,
    const ButtonSpec& button,
    float text_points,
    float pad_points,
    float least_points
) {
    const int32_t size = canvas_font(type, text_points);
    const int32_t natural = measured_width(type.measure, button.label, size, true) +
                            mark_room(type, button, size) + 2 * canvas_pixels(type, pad_points);
    return std::max(natural, canvas_pixels(type, least_points));
}

void button_part(
    std::vector<Item>& parts,
    const Typesetter& type,
    const Rect& box,
    const ButtonSpec& button,
    float text_points
) {
    Item part{};
    part.role = button.role;
    part.rect = box;
    part.style = TextStyle::button;
    part.pixel_size = canvas_font(type, text_points);
    part.bold = true;
    part.colour = button.role == Role::button_main     ? screen_colour::ink
                  : button.role == Role::button_danger ? screen_colour::red
                                                       : screen_colour::button_text;
    part.glyph = button.glyph;
    if (button.glyph != Glyph::none && button.glyph_points > 0.0F)
        part.glyph_size = canvas_pixels(type, button.glyph_points);
    part.control = button.control;
    part.state.disabled = !button.enabled;
    part.state.on = button.role == Role::button_main;
    const int32_t size = part.pixel_size;
    const int32_t room = box.width - mark_room(type, button, size) - 2 * canvas_pixels(type, 4.0F);
    std::string label = fit(button.label, room, [&type, size](std::string_view shown) {
        return measured_width(type.measure, shown, size, true);
    });
    if (!label.empty())
        part.lines = {std::move(label)};
    parts.push_back(std::move(part));
}

int32_t button_row(
    std::vector<Item>& parts,
    const Typesetter& type,
    Point at,
    int32_t room,
    std::span<const ButtonSpec> left,
    std::span<const ButtonSpec> right,
    const ButtonRowSizes& sizes
) {
    const int32_t height = canvas_pixels(type, sizes.height);
    const int32_t gap = canvas_pixels(type, sizes.gap);
    std::vector<int32_t> widths;
    int32_t total = 0;
    const auto measure_button = [&](const ButtonSpec& button) {
        const float least = button.role == Role::button_main ? sizes.least_main : 0.0F;
        widths.push_back(std::min(room, button_span(type, button, sizes.text, sizes.pad, least)));
        total += widths.back() + (widths.size() > 1 ? gap : 0);
    };
    for (const ButtonSpec& button : left)
        measure_button(button);
    for (const ButtonSpec& button : right)
        measure_button(button);
    if (widths.empty())
        return 0;
    if (!left.empty() && !right.empty())
        total += gap * 2; // the space between the two groups
    if (total <= room) {
        int32_t column = at.x;
        for (std::size_t index = 0; index < left.size(); ++index) {
            button_part(
                parts, type, {column, at.y, widths[index], height}, left[index], sizes.text
            );
            column += widths[index] + gap;
        }
        int32_t right_width = 0;
        for (std::size_t index = 0; index < right.size(); ++index)
            right_width += widths[left.size() + index] + (index > 0 ? gap : 0);
        column = at.x + room - right_width;
        for (std::size_t index = 0; index < right.size(); ++index) {
            const int32_t width = widths[left.size() + index];
            button_part(parts, type, {column, at.y, width, height}, right[index], sizes.text);
            column += width + gap;
        }
        return height;
    }
    int32_t column = at.x;
    int32_t top = at.y;
    std::size_t index = 0;
    const auto place = [&](const ButtonSpec& button) {
        const int32_t width = widths[index++];
        if (column > at.x && column + width > at.x + room) {
            column = at.x;
            top += height + gap;
        }
        button_part(parts, type, {column, top, width, height}, button, sizes.text);
        column += width + gap;
    };
    for (const ButtonSpec& button : left)
        place(button);
    for (const ButtonSpec& button : right)
        place(button);
    return top + height - at.y;
}

void icon_part(std::vector<Item>& parts, const Rect& box, Glyph glyph, Colour colour, bool framed) {
    if (glyph == Glyph::none && !framed)
        return;
    Item part{};
    part.role = Role::icon;
    part.rect = box;
    part.glyph = glyph;
    part.colour = colour;
    part.state.on = framed;
    parts.push_back(std::move(part));
}

void plain_part(std::vector<Item>& parts, Role role, const Rect& box, Colour colour) {
    Item part{};
    part.role = role;
    part.rect = box;
    part.colour = colour;
    parts.push_back(std::move(part));
}

void move_parts(std::vector<Item>& to, std::vector<Item>& from, int32_t down) {
    for (Item& part : from) {
        part.rect.y += down;
        to.push_back(std::move(part));
    }
    from.clear();
}

void move_parts(std::vector<Item>& to, std::vector<Item>& from, int32_t down, const Rect& clip) {
    for (Item& part : from) {
        part.rect.y += down;
        part.clip = clip;
        to.push_back(std::move(part));
    }
    from.clear();
}

ScrollColumn scroll_column(
    const Rect& area,
    int32_t top_height,
    int32_t body_height,
    int32_t bottom_height,
    int32_t gap,
    int32_t least_body,
    int32_t offset
) noexcept {
    const int32_t between = body_height > 0 && bottom_height > 0 ? gap : 0;
    const int32_t natural = top_height + body_height + between + bottom_height;
    const int32_t wanted = std::max(int32_t{0}, offset);
    ScrollColumn placed{};
    if (natural <= area.height) {
        placed.top = area.y;
        placed.body = area.y + top_height;
        placed.bottom = area.y + top_height + body_height + between;
        placed.scroll.content_height = natural;
        return placed;
    }
    const int32_t body_room = area.height - top_height - bottom_height - between;
    if (body_height > 0 && body_room >= least_body) {
        const Rect view{area.x, area.y + top_height, area.width, body_room};
        placed.scroll.view = view;
        placed.scroll.content_height = body_height;
        placed.scroll.limit = body_height - body_room;
        placed.scroll.offset = std::min(wanted, placed.scroll.limit);
        placed.top = area.y;
        placed.body = view.y - placed.scroll.offset;
        placed.body_clip = view;
        placed.bottom = view.y + body_room + between;
        return placed;
    }
    placed.scroll.view = area;
    placed.scroll.content_height = natural;
    placed.scroll.limit = natural - area.height;
    placed.scroll.offset = std::min(wanted, placed.scroll.limit);
    placed.top = area.y - placed.scroll.offset;
    placed.body = area.y + top_height - placed.scroll.offset;
    placed.bottom = area.y + top_height + body_height + between - placed.scroll.offset;
    placed.top_clip = area;
    placed.body_clip = area;
    placed.bottom_clip = area;
    return placed;
}

std::size_t first_live_part(const DisplayList& list) noexcept {
    std::size_t first = 0;
    for (std::size_t index = 0; index < list.items.size(); ++index)
        if (list.items[index].role == Role::backdrop)
            first = index + 1;
    return first;
}

void list_part_controls(DisplayList& list) {
    list.controls.clear();
    list.tab_order.clear();
    const std::size_t first = first_live_part(list);
    for (std::size_t index = list.items.size(); index-- > first;) {
        const Item& part = list.items[index];
        if (part.control == no_control)
            continue;
        Control control{};
        control.id = part.control;
        control.rect = part.rect;
        control.clip = part.clip;
        control.enabled = !part.state.disabled;
        control.kind = part.role == Role::toggle ? ControlKind::toggle : ControlKind::button;
        control.checked = part.role == Role::toggle && part.state.on;
        if (!part.lines.empty())
            control.text = part.lines.front();
        list.controls.push_back(std::move(control));
    }
    for (std::size_t index = first; index < list.items.size(); ++index) {
        const Item& part = list.items[index];
        if (part.control == no_control || part.state.disabled)
            continue;
        if (std::find(list.tab_order.begin(), list.tab_order.end(), part.control) ==
            list.tab_order.end())
            list.tab_order.push_back(part.control);
    }
}

} // namespace oa::ui::kit
