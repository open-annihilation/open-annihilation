// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/ui/gui_input/scroll_bar.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <variant>

namespace oa::ui::gui_input {

namespace {

namespace attribute = ui::gui_layout::attribute;
using ui::gui_layout::GadgetRect;

// A horizontal bar's range is its width less its knob and this margin.
constexpr int32_t kKnobRangeMargin = 4;
// A bar with no art has a range of its longer side less this margin.
constexpr int32_t kArtlessRangeMargin = 6;
// A press grabs a knob in a rectangle that starts this far into the bar
// across it, and past a horizontal knob's position along it; a vertical
// knob's starts the second distance past its position.
constexpr int32_t kKnobGrabInset = 1;
constexpr int32_t kVerticalKnobGrabInset = 2;
// Across the bar, the rectangle's far edge lies the bar's thickness less
// this past its near edge.
constexpr int32_t kKnobGrabThicknessLess = 2;
// The smallest knob a text list's bar is sized to, in pixels.
constexpr int32_t kMinimumListKnob = 10;
// A text list's knob and range leave this much of the bar's length out.
constexpr int32_t kListKnobMargin = 3;
// A list's rows start this far below its top; a page is its height less this.
constexpr int32_t kListRowsInset = 2;
// A press takes a row down to this far above the list's bottom.
constexpr int32_t kListRowsBottomInset = 4;

/// Returns a frame's size; a frame past the sequence's end has none.
ScrollFrame frame_size(const std::vector<ScrollFrame>& frames, int32_t frame) noexcept {
    return frame >= 0 && static_cast<std::size_t>(frame) < frames.size()
               ? frames[static_cast<std::size_t>(frame)]
               : ScrollFrame{};
}

/// Tells whether a point lies in a rectangle, its right and bottom edges excluded.
bool inside(const ScrollRect& rect, int32_t x, int32_t y) noexcept {
    return rect.width > 0 && rect.height > 0 && x >= rect.x && y >= rect.y &&
           x < rect.x + rect.width && y < rect.y + rect.height;
}

/// Tells whether a bar's knob moves across, rather than down.
bool horizontal(const ScrollBar& bar) noexcept {
    return (bar.attributes & attribute::horizontal) != 0;
}

/// Keeps a bar's knob within 0..range - 1.
void clamp_knob(ScrollBar& bar) noexcept {
    if (bar.range - 1 < bar.knob)
        bar.knob = static_cast<int16_t>(bar.range - 1);
    if (bar.knob < 0)
        bar.knob = 0;
}

/// Moves a bar's knob one position toward a point before or after it.
///
/// @return true when the knob moved
bool step_toward(ScrollBar& bar, int32_t x, int32_t y) noexcept {
    const auto before = bar.knob;
    const auto knob = scroll_knob_rect(bar);
    const int32_t along = horizontal(bar) ? x : y;
    const int32_t start = horizontal(bar) ? knob.left : knob.top;
    const int32_t end = horizontal(bar) ? knob.right : knob.bottom;
    if (along < start)
        bar.knob = static_cast<int16_t>(before - 1);
    else if (along > end)
        bar.knob = static_cast<int16_t>(before + 1);
    clamp_knob(bar);
    return bar.knob != before;
}

/// Returns the first row a knob shows of a list: hidden * knob / (range - 1), truncated.
int32_t first_row_for_knob(int32_t hidden, const ScrollBar& bar) noexcept {
    if (bar.range < 2)
        return 0;
    const double row = static_cast<double>(hidden) * static_cast<double>(bar.knob) /
                       static_cast<double>(bar.range - 1);
    return std::max(0, static_cast<int32_t>(row));
}

} // namespace

ScrollBar scroll_bar_from_gadget(const ui::gui_layout::Gadget& gadget) noexcept {
    ScrollBar bar;
    bar.rect = {gadget.common.x, gadget.common.y, gadget.common.width, gadget.common.height};
    bar.attributes = static_cast<uint32_t>(gadget.common.attributes);
    bar.group = gadget.common.association;
    bar.active = gadget.common.active != 0;
    if (const auto* fields = std::get_if<ui::gui_layout::ScrollBarFields>(&gadget.fields)) {
        bar.range = fields->range;
        bar.knob = fields->knob_position;
        bar.knob_size = fields->knob_size;
        bar.maximum = fields->thickness;
        bar.grayed = fields->locked;
    }
    return bar;
}

void bind_scroll_bar(ScrollBar& bar, const ScrollArtFrames& art) noexcept {
    bar.back_arrow = {};
    bar.forward_arrow = {};
    bar.knob = 0;
    const std::vector<ScrollFrame>* frames = nullptr;
    if (!art.panel.empty()) {
        frames = &art.panel;
        bar.art = ScrollArt::panel;
        bar.art_base = 0;
    } else if (!art.shared.empty()) {
        frames = &art.shared;
        bar.art = ScrollArt::shared;
        bar.art_base = bar.rect.height < bar.rect.width ? kScrollHorizontalArt : 0;
        if (bar.art_base < frames->size()) {
            const auto first = (*frames)[bar.art_base];
            if (bar.rect.width < bar.rect.height)
                bar.rect.width = first.width;
            else
                bar.rect.height = first.height;
        }
    } else {
        bar.art = ScrollArt::none;
        bar.range =
            static_cast<int16_t>(std::max(bar.rect.width, bar.rect.height) - kArtlessRangeMargin);
        return;
    }
    const int32_t base = bar.art_base;
    const auto back = frame_size(*frames, base + kScrollBackArrow);
    const auto forward = frame_size(*frames, base + kScrollForwardArrow);
    bar.back_arrow = {bar.rect.x, bar.rect.y, back.width, back.height};
    bar.forward_arrow = {bar.rect.x, bar.rect.y, forward.width, forward.height};
    if (bar.rect.height < bar.rect.width) {
        bar.forward_arrow.x = static_cast<int16_t>(bar.rect.width - back.width + bar.rect.x);
        bar.rect.width = static_cast<int16_t>(bar.rect.width - 2 * back.width);
        bar.rect.x = static_cast<int16_t>(bar.rect.x + back.width);
        // The knob is as wide as its end frame.
        bar.knob_size = frame_size(*frames, base + kScrollKnobEnd).width;
        bar.range = static_cast<int16_t>(bar.rect.width - bar.knob_size - kKnobRangeMargin);
    } else {
        bar.forward_arrow.y = static_cast<int16_t>(bar.rect.y - back.height + bar.rect.height);
        bar.rect.height = static_cast<int16_t>(bar.rect.height - 2 * back.height);
        bar.rect.y = static_cast<int16_t>(bar.rect.y + back.height);
    }
}

ScrollPart scroll_bar_part(const ScrollBar& bar, int32_t x, int32_t y) noexcept {
    if (!bar.active || bar.art == ScrollArt::unbound)
        return ScrollPart::none;
    if (inside(bar.back_arrow, x, y))
        return ScrollPart::back_arrow;
    if (inside(bar.forward_arrow, x, y))
        return ScrollPart::forward_arrow;
    const GadgetRect outer{
        bar.rect.x, bar.rect.y, bar.rect.x + bar.rect.width, bar.rect.y + bar.rect.height
    };
    return ui::gui_layout::rect_contains(outer, x, y) ? ScrollPart::bar : ScrollPart::none;
}

GadgetRect scroll_knob_rect(const ScrollBar& bar) noexcept {
    if (horizontal(bar)) {
        const int32_t left = bar.knob + kKnobGrabInset + bar.rect.x;
        const int32_t top = bar.rect.y + kKnobGrabInset;
        return {left, top, left + bar.knob_size, top + bar.rect.height - kKnobGrabThicknessLess};
    }
    const int32_t left = bar.rect.x + kKnobGrabInset;
    const int32_t top = bar.knob + kVerticalKnobGrabInset + bar.rect.y;
    return {left, top, left + bar.rect.width - kKnobGrabThicknessLess, top + bar.knob_size};
}

bool scroll_takes_input(const ScrollBar& bar) noexcept {
    return bar.art != ScrollArt::unbound && bar.active && !bar.grayed &&
           (bar.attributes & attribute::text_list) == 0;
}

void scroll_set_value(ScrollBar& bar, int32_t value) noexcept {
    if (bar.maximum <= 0 || bar.range < 2) {
        bar.knob = 0;
        return;
    }
    const auto lowered = std::min(value, bar.maximum);
    const double position = static_cast<double>(lowered) / static_cast<double>(bar.maximum) *
                            static_cast<double>(bar.range - 1);
    const double whole = std::trunc(position);
    bar.knob = static_cast<int16_t>(position == whole ? whole : std::trunc(position + 1.0));
}

int32_t scroll_value(const ScrollBar& bar) noexcept {
    if (bar.range < 2)
        return 0;
    const double value = static_cast<double>(bar.knob) / static_cast<double>(bar.range - 1) *
                         static_cast<double>(bar.maximum);
    return static_cast<int32_t>(value);
}

void scroll_step(ScrollBar& bar, bool forward) noexcept {
    const auto knob = bar.knob;
    if (forward) {
        if (knob < bar.range - 1)
            bar.knob = static_cast<int16_t>(knob + 1);
    } else if (knob > 0) {
        bar.knob = static_cast<int16_t>(knob - 1);
    }
}

int16_t scroll_page_span(int16_t range) noexcept {
    if (range < 2)
        return 1;
    const int32_t span = (static_cast<int32_t>(range) - 1) / 10;
    return static_cast<int16_t>(span < 1 ? 1 : span);
}

void scroll_let_go(ScrollHold& hold) noexcept {
    const auto repeat_tick = hold.repeat_tick;
    hold = ScrollHold{};
    hold.repeat_tick = repeat_tick;
}

bool scroll_press(
    ScrollBar& bar, ScrollHold& hold, int32_t id, int32_t x, int32_t y, uint32_t tick
) noexcept {
    scroll_let_go(hold);
    hold.pointer_x = x;
    hold.pointer_y = y;
    const auto part = scroll_bar_part(bar, x, y);
    if (part == ScrollPart::none || !scroll_takes_input(bar))
        return false;
    hold.bar = id;
    hold.part = part;
    if (part != ScrollPart::bar) {
        hold.repeat_delay = kScrollRepeatDelay;
        scroll_step(bar, part == ScrollPart::forward_arrow);
        return true;
    }
    hold.step_tick = tick;
    if (ui::gui_layout::rect_contains(scroll_knob_rect(bar), x, y)) {
        hold.dragging = true;
        hold.drag_origin = horizontal(bar) ? x : y;
        hold.drag_knob = bar.knob;
    }
    return false;
}

void scroll_move(ScrollHold& hold, int32_t x, int32_t y) noexcept {
    hold.pointer_x = x;
    hold.pointer_y = y;
}

bool scroll_release(ScrollBar& bar, ScrollHold& hold, int32_t x, int32_t y) noexcept {
    const bool held_bar = hold.bar != kNoScrollBar && hold.part == ScrollPart::bar;
    scroll_let_go(hold);
    return held_bar && scroll_takes_input(bar) && step_toward(bar, x, y);
}

bool scroll_hold_tick(ScrollBar& bar, ScrollHold& hold, uint32_t tick) noexcept {
    if (hold.bar == kNoScrollBar)
        return false;
    if (!scroll_takes_input(bar)) {
        scroll_let_go(hold);
        return false;
    }
    if (hold.part != ScrollPart::bar) {
        if (tick == hold.repeat_tick)
            return false;
        hold.repeat_tick = tick;
        if (hold.repeat_delay >= 1) {
            --hold.repeat_delay;
            return false;
        }
        scroll_step(bar, hold.part == ScrollPart::forward_arrow);
        return true;
    }
    const auto before = bar.knob;
    if (hold.dragging) {
        const int32_t along = horizontal(bar) ? hold.pointer_x : hold.pointer_y;
        bar.knob = static_cast<int16_t>(hold.drag_knob + (along - hold.drag_origin));
        clamp_knob(bar);
    } else if (tick != hold.step_tick) {
        hold.step_tick = tick;
        (void)step_toward(bar, hold.pointer_x, hold.pointer_y);
    }
    return bar.knob != before;
}

ScrollList scroll_list_from_gadget(const ui::gui_layout::Gadget& gadget) noexcept {
    ScrollList list;
    list.y = gadget.common.y;
    list.height = gadget.common.height;
    list.group = gadget.common.association;
    list.active = gadget.common.active != 0;
    if (const auto* fields = std::get_if<ui::gui_layout::ListBoxFields>(&gadget.fields))
        list.item_height = fields->item_height;
    return list;
}

void scroll_list_trim(ScrollList& list, int32_t line_height) noexcept {
    if (!list.active)
        return;
    list.first = 0;
    list.selection = 0;
    if (line_height > 0) {
        const int32_t pitch = line_height + 2;
        list.height = static_cast<int16_t>(list.height - list.height % pitch);
    }
}

int32_t scroll_list_pitch(const ScrollList& list, int32_t line_height) noexcept {
    const int32_t pitch = list.item_height != 0 ? list.item_height : line_height + 1;
    return std::max(1, pitch);
}

int32_t scroll_list_page_rows(const ScrollList& list, int32_t line_height) noexcept {
    return (list.height - kListRowsInset) / std::max(1, line_height + 1);
}

bool scroll_list_fill(ScrollList& list, int32_t count, int32_t line_height) noexcept {
    list.count =
        static_cast<int16_t>(std::clamp<int32_t>(count, 0, std::numeric_limits<int16_t>::max()));
    list.item_height = static_cast<int16_t>(std::max<int32_t>(list.item_height, line_height + 1));
    list.first = 0;
    list.selection = 0;
    const int32_t pitch = scroll_list_pitch(list, line_height);
    int32_t last_first = list.count - 1;
    int32_t remaining = list.height;
    for (int32_t row = list.count - 1; row >= 0; --row) {
        remaining -= pitch;
        if (remaining < 0)
            break;
        last_first = row;
    }
    list.last_first = static_cast<int16_t>(last_first);
    return remaining < 0;
}

void scroll_bar_fit_list(ScrollBar& bar, const ScrollList& list, int32_t line_height) noexcept {
    const int32_t pitch = std::max<int32_t>({list.item_height, line_height + 1, int32_t{1}});
    const int32_t rows = (list.height - kListRowsInset) / pitch;
    const int32_t count = list.count;
    int32_t knob = 0;
    if (count > 0)
        knob = static_cast<int32_t>(
            static_cast<double>(rows) / static_cast<double>(count) *
            static_cast<double>(bar.rect.height - kListKnobMargin)
        );
    bar.knob_size = static_cast<int16_t>(std::max(knob, kMinimumListKnob));
    bar.range =
        static_cast<int16_t>(count > rows ? bar.rect.height - bar.knob_size - kListKnobMargin : 0);
}

bool scroll_list_follow_bar(ScrollList& list, const ScrollBar& bar) noexcept {
    if (list.item_height == 0)
        return false;
    const int32_t visible = list.height / list.item_height;
    list.first = static_cast<int16_t>(first_row_for_knob(list.count - visible, bar));
    return true;
}

void scroll_bar_follow_list(ScrollBar& bar, const ScrollList& list) noexcept {
    if (list.count <= 1)
        return;
    int32_t knob = 0;
    if (list.last_first != 0)
        knob = static_cast<int32_t>(
            static_cast<double>(list.first) * static_cast<double>(bar.range) /
            static_cast<double>(list.last_first)
        );
    bar.knob = static_cast<int16_t>(knob);
}

void scroll_list_select(
    ScrollList& list, ScrollBar* bar, int32_t line_height, int32_t row
) noexcept {
    list.selection = static_cast<int16_t>(row);
    const int32_t rows = scroll_list_page_rows(list, line_height);
    if (row >= list.first && row <= list.first + rows - 1)
        return;
    if (list.last_first != 0)
        list.first = static_cast<int16_t>(row);
    if (list.last_first < list.first)
        list.first = std::max<int16_t>(0, list.last_first);
    if (bar == nullptr || list.last_first <= 0)
        return;
    bar->knob = static_cast<int16_t>(
        static_cast<double>(bar->range) * static_cast<double>(list.first) /
        static_cast<double>(list.last_first)
    );
}

bool scroll_list_step(
    ScrollList& list, ScrollBar* bar, int32_t line_height, bool forward
) noexcept {
    const int32_t count = list.count;
    if (count == 0)
        return false;
    const int32_t rows = scroll_list_page_rows(list, line_height);
    const int32_t first = list.first;
    const int32_t selected = list.selection;
    if (selected < first || selected >= first + rows) {
        scroll_list_select(list, bar, line_height, selected);
        return false;
    }
    if (forward) {
        if (selected == list.last_first - 1 + rows)
            return false;
        const int32_t row = selected + 1;
        if (row > first + rows - 1)
            list.first = static_cast<int16_t>(first + 1);
        list.selection = static_cast<int16_t>(std::min(row, count - 1));
    } else {
        if (selected == 0)
            return false;
        const int32_t row = selected - 1;
        if (row < first)
            list.first = static_cast<int16_t>(first - 1);
        list.selection = static_cast<int16_t>(row);
    }
    return true;
}

ScrollListPress
scroll_list_press(const ScrollList& list, int32_t line_height, int32_t y, int32_t& row) noexcept {
    const int32_t count = list.count;
    const int32_t top = list.y + kListRowsInset;
    if (count == 0 || y < top || y > list.y + list.height - kListRowsBottomInset)
        return ScrollListPress::missed;
    const int32_t pitch = scroll_list_pitch(list, line_height);
    const int32_t rows = (list.height - kListRowsInset) / pitch;
    int32_t picked = (y - top) / pitch + list.first;
    picked = std::min(picked, list.first + rows - 1);
    picked = std::max(0, std::min(picked, count - 1));
    row = picked;
    return picked == list.selection ? ScrollListPress::same : ScrollListPress::changed;
}

bool scroll_list_scroll(ScrollList& list, int32_t rows) noexcept {
    if (list.item_height == 0)
        return false;
    const int32_t last = std::max<int32_t>(0, list.last_first);
    const int32_t first = std::clamp<int32_t>(list.first + rows, 0, last);
    if (first == list.first)
        return false;
    list.first = static_cast<int16_t>(first);
    return true;
}

} // namespace oa::ui::gui_input
