// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Pointer, finger reach, keys, two-dimensional focus and the wheel, over a
// kit screen's display list, and the editing of a text field.

#include "oa/ui/kit/input.hpp"

#include "oa/present/game_text.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/looks.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdint.h>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace oa::ui::kit {

namespace {

/// Returns the part of a control a press can reach.
///
/// An empty clip is no clip: the whole rectangle. Otherwise the part the
/// rectangle shares with the clip.
///
/// @param control the control
/// @return the press area, empty when the clip hides it all
[[nodiscard]] Rect press_area(const Control& control) noexcept {
    const bool clipped = control.clip.width > 0 && control.clip.height > 0;
    if (!clipped)
        return control.rect;
    return intersect(control.rect, control.clip);
}

/// Tells whether a scroll area shows any of a control.
///
/// @param control the control
/// @return true when its press area has room
[[nodiscard]] bool in_view(const Control& control) noexcept {
    const Rect area = press_area(control);
    return area.width > 0 && area.height > 0;
}

/// Returns the square of the distance between two points.
///
/// @param from one point
/// @param to the other
/// @return the distance squared, in points squared
[[nodiscard]] int64_t distance_squared(Point from, Point to) noexcept {
    const int64_t across = int64_t{from.x} - to.x;
    const int64_t down = int64_t{from.y} - to.y;
    return across * across + down * down;
}

/// Returns a point moved by a finger's shift.
///
/// @param point the pointer
/// @param shift how far the press was moved
/// @return the point the held press uses
[[nodiscard]] Point shifted(Point point, Point shift) noexcept {
    return {point.x + shift.x, point.y + shift.y};
}

/// Returns the composite whose id is a tab stop.
///
/// @param list the display list
/// @param id the stop
/// @return that composite, or null when id is not one
[[nodiscard]] const Composite* composite_by_id(const DisplayList& list, ControlId id) noexcept {
    for (const Composite& composite : list.composites)
        if (composite.id == id)
            return &composite;
    return nullptr;
}

/// Returns the composite that lists a control as a part.
///
/// @param list the display list
/// @param id the control
/// @return that composite, or null when the control is not a part
[[nodiscard]] const Composite* composite_of_part(const DisplayList& list, ControlId id) noexcept {
    for (const Composite& composite : list.composites)
        for (const ControlId part : composite.parts)
            if (part == id)
                return &composite;
    return nullptr;
}

/// Returns a part's place in a composite, from 0.
///
/// @param composite the group
/// @param id the control
/// @return its place, or -1 when it is not a part
[[nodiscard]] int32_t index_in(const Composite& composite, ControlId id) noexcept {
    const auto count = static_cast<int32_t>(composite.parts.size());
    for (int32_t index = 0; index < count; ++index)
        if (composite.parts[static_cast<std::size_t>(index)] == id)
            return index;
    return -1;
}

/// Tells whether a part takes the focus: it exists, and it is focusable and enabled.
///
/// @param list the display list
/// @param id the part
/// @return true when the arrows may stop on it
[[nodiscard]] bool part_live(const DisplayList& list, ControlId id) noexcept {
    const Control* control = control_of(list, id);
    return control != nullptr && control->focusable && control->enabled;
}

/// Returns the part a composite lands on, without remembering it.
///
/// The remembered part when it takes the focus, else the chosen part, else
/// the first that takes the focus, else the first part. An empty composite
/// lands on no control.
///
/// @param list the display list
/// @param composite the group
/// @return the part, or no_control when it has none
[[nodiscard]] ControlId landing_part(const DisplayList& list, const Composite& composite) noexcept {
    if (part_live(list, composite.current) && index_in(composite, composite.current) >= 0)
        return composite.current;
    ControlId checked = no_control;
    ControlId first_live = no_control;
    for (const ControlId part : composite.parts) {
        if (!part_live(list, part))
            continue;
        if (first_live == no_control)
            first_live = part;
        const Control* control = control_of(list, part);
        if (checked == no_control && control != nullptr && control->checked)
            checked = part;
    }
    if (checked != no_control)
        return checked;
    if (first_live != no_control)
        return first_live;
    if (composite.parts.empty())
        return no_control;
    return composite.parts.front();
}

/// Remembers a part as the one its composite lands on.
///
/// @param list the display list
/// @param id the control; a control that is not a part changes nothing
void remember_part(const DisplayList& list, ControlId id) noexcept {
    if (const Composite* composite = composite_of_part(list, id))
        composite->current = id;
}

/// Where a control sits in the declared order. A part sits on its composite's stop.
struct OrderPlace {
    bool found{false};    ///< the control is a stop, or a part of one
    std::size_t index{0}; ///< that stop's place, from 0
};

/// Returns where a control sits in the declared order.
///
/// @param list the display list
/// @param from the control
/// @return found when it is a stop or a part of a composite that is a stop
[[nodiscard]] OrderPlace order_place(const DisplayList& list, ControlId from) noexcept {
    const auto& order = list.tab_order;
    const auto direct = std::find(order.begin(), order.end(), from);
    if (direct != order.end())
        return {true, static_cast<std::size_t>(direct - order.begin())};
    if (const Composite* group = composite_of_part(list, from)) {
        const auto stop = std::find(order.begin(), order.end(), group->id);
        if (stop != order.end())
            return {true, static_cast<std::size_t>(stop - order.begin())};
    }
    return {};
}

/// Returns the rectangle that holds every part that is a control.
///
/// @param list the display list
/// @param composite the group
/// @param[out] united the rectangle, when any part is a control
/// @return true when a part is a control
[[nodiscard]] bool
united_rect(const DisplayList& list, const Composite& composite, Rect& united) noexcept {
    bool any = false;
    int32_t left = 0;
    int32_t top = 0;
    int32_t right = 0;
    int32_t bottom = 0;
    for (const ControlId part : composite.parts) {
        const Control* control = control_of(list, part);
        if (control == nullptr)
            continue;
        const int32_t part_right = control->rect.x + control->rect.width;
        const int32_t part_bottom = control->rect.y + control->rect.height;
        if (!any) {
            left = control->rect.x;
            top = control->rect.y;
            right = part_right;
            bottom = part_bottom;
            any = true;
            continue;
        }
        left = std::min(left, control->rect.x);
        top = std::min(top, control->rect.y);
        right = std::max(right, part_right);
        bottom = std::max(bottom, part_bottom);
    }
    if (!any)
        return false;
    united = {left, top, right - left, bottom - top};
    return true;
}

/// Tells whether a scroll area shows any part of a composite.
///
/// @param list the display list
/// @param composite the group
/// @return true when a part's press area has room
[[nodiscard]] bool any_in_view(const DisplayList& list, const Composite& composite) noexcept {
    for (const ControlId part : composite.parts) {
        const Control* control = control_of(list, part);
        if (control != nullptr && in_view(*control))
            return true;
    }
    return false;
}

/// Tells whether the focus may stop on a control.
///
/// A part of a composite whose id is in the declared order may, as well as
/// a control that is in the order itself.
///
/// @param list the display list
/// @param id the control
/// @return true when it is focusable and in the declared order
[[nodiscard]] bool takes_focus(const DisplayList& list, ControlId id) noexcept {
    const Control* control = control_of(list, id);
    if (control == nullptr || !control->focusable)
        return false;
    for (const ControlId ordered : list.tab_order)
        if (ordered == id)
            return true;
    if (const Composite* group = composite_of_part(list, id))
        for (const ControlId ordered : list.tab_order)
            if (ordered == group->id)
                return true;
    return false;
}

/// Returns a rectangle's centre on one axis, doubled so a half point is exact.
///
/// @param origin the rectangle's start on the axis
/// @param extent the rectangle's length on the axis
/// @return twice the centre
[[nodiscard]] int64_t centre_of(int32_t origin, int32_t extent) noexcept {
    return static_cast<int64_t>(origin) * 2 + extent;
}

/// Tells whether two rectangles share any point of their area.
///
/// @param a a rectangle
/// @param b another
/// @return true when they overlap
[[nodiscard]] bool overlaps(const Rect& a, const Rect& b) noexcept {
    return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height &&
           b.y < a.y + a.height;
}

/// A span on one axis, its end outside it.
struct Span {
    int32_t begin{}; ///< the first point
    int32_t end{};   ///< the point after the last
};

/// Returns a rectangle's span across a direction: the axis the direction does not move on.
///
/// @param rect the rectangle
/// @param direction the way the focus moves
/// @return the span
[[nodiscard]] Span across_span(const Rect& rect, Direction direction) noexcept {
    if (direction == Direction::up || direction == Direction::down)
        return {rect.x, rect.x + rect.width};
    return {rect.y, rect.y + rect.height};
}

/// Tells whether two spans share any point.
///
/// @param a a span
/// @param b another
/// @return true when they overlap
[[nodiscard]] bool spans_overlap(Span a, Span b) noexcept {
    return a.begin < b.end && b.begin < a.end;
}

/// Returns the gap between two spans, 0 when they overlap or touch.
///
/// @param a a span
/// @param b another
/// @return the points between them
[[nodiscard]] int64_t span_gap(Span a, Span b) noexcept {
    if (a.end <= b.begin)
        return static_cast<int64_t>(b.begin) - a.end;
    if (b.end <= a.begin)
        return static_cast<int64_t>(a.begin) - b.end;
    return 0;
}

/// Tells whether a candidate's centre lies strictly past another's in a direction.
///
/// @param from the focused control's rectangle
/// @param candidate the candidate's rectangle
/// @param direction the way the focus moves
/// @return true when the candidate's centre is strictly that way
[[nodiscard]] bool
centre_past(const Rect& from, const Rect& candidate, Direction direction) noexcept {
    const bool vertical = direction == Direction::up || direction == Direction::down;
    const bool forward = direction == Direction::down || direction == Direction::right;
    const int64_t from_centre =
        vertical ? centre_of(from.y, from.height) : centre_of(from.x, from.width);
    const int64_t candidate_centre = vertical ? centre_of(candidate.y, candidate.height)
                                              : centre_of(candidate.x, candidate.width);
    return forward ? candidate_centre > from_centre : candidate_centre < from_centre;
}

/// Tells whether a candidate lies wholly past a rectangle's facing edge.
///
/// Touching the edge counts as past.
///
/// @param from the focused control's rectangle
/// @param candidate the candidate's rectangle
/// @param direction the way the focus moves
/// @return true when the candidate is wholly that way
[[nodiscard]] bool
wholly_past(const Rect& from, const Rect& candidate, Direction direction) noexcept {
    switch (direction) {
    case Direction::right:
        return int64_t{candidate.x} >= int64_t{from.x} + from.width;
    case Direction::left:
        return int64_t{candidate.x} + candidate.width <= from.x;
    case Direction::down:
        return int64_t{candidate.y} >= int64_t{from.y} + from.height;
    case Direction::up:
        return int64_t{candidate.y} + candidate.height <= from.y;
    }
    return false;
}

/// Tells whether a candidate lies beyond the focused control in a direction.
///
/// @param from the focused control's rectangle
/// @param candidate the candidate's rectangle
/// @param direction the way the focus moves
/// @return true when the focus can move to it that way
[[nodiscard]] bool beyond(const Rect& from, const Rect& candidate, Direction direction) noexcept {
    if (wholly_in(candidate, from) || wholly_in(from, candidate))
        return centre_past(from, candidate, direction);
    if (overlaps(from, candidate))
        return false;
    return wholly_past(from, candidate, direction);
}

/// Returns the gap from a rectangle's facing edge to a candidate's near edge.
///
/// Negative when the candidate starts before that edge, as a control inside
/// another does.
///
/// @param from the focused control's rectangle
/// @param candidate the candidate's rectangle
/// @param direction the way the focus moves
/// @return the gap, in points
[[nodiscard]] int64_t
gap_along(const Rect& from, const Rect& candidate, Direction direction) noexcept {
    switch (direction) {
    case Direction::right:
        return static_cast<int64_t>(candidate.x) - (static_cast<int64_t>(from.x) + from.width);
    case Direction::left:
        return static_cast<int64_t>(from.x) - (static_cast<int64_t>(candidate.x) + candidate.width);
    case Direction::down:
        return static_cast<int64_t>(candidate.y) - (static_cast<int64_t>(from.y) + from.height);
    case Direction::up:
        return static_cast<int64_t>(from.y) -
               (static_cast<int64_t>(candidate.y) + candidate.height);
    }
    return 0;
}

/// Returns the distance between two rectangles' centres across a direction.
///
/// @param from the focused control's rectangle
/// @param candidate the candidate's rectangle
/// @param direction the way the focus moves
/// @return the distance, in half-points
[[nodiscard]] int64_t
centre_across(const Rect& from, const Rect& candidate, Direction direction) noexcept {
    const bool vertical = direction == Direction::up || direction == Direction::down;
    const int64_t from_centre =
        vertical ? centre_of(from.x, from.width) : centre_of(from.y, from.height);
    const int64_t candidate_centre = vertical ? centre_of(candidate.x, candidate.width)
                                              : centre_of(candidate.y, candidate.height);
    const int64_t difference = candidate_centre - from_centre;
    return difference < 0 ? -difference : difference;
}

/// Tells whether a name is words of a-z, 0-9 and hyphens joined by dots.
///
/// @param name the name
/// @return true when it matches
[[nodiscard]] bool name_words(std::string_view name) noexcept {
    if (name.empty())
        return false;
    bool word = false;
    for (const char character : name) {
        const bool letter = character >= 'a' && character <= 'z';
        const bool digit = character >= '0' && character <= '9';
        if (letter || digit || character == '-') {
            word = true;
            continue;
        }
        if (character == '.' && word) {
            word = false;
            continue;
        }
        return false;
    }
    return word;
}

/// Tells whether a rectangle holds any point.
///
/// @param rect the rectangle
/// @return true when it is wide and high
[[nodiscard]] bool has_room(const Rect& rect) noexcept {
    return rect.width > 0 && rect.height > 0;
}

/// One part of a control that takes a press of its own, as automation names it.
struct Part {
    std::string word;   ///< the word after its control's name
    Rect rect{};        ///< where it lies, in points
    bool inside{true};  ///< it lies inside its control, whose press area clips it
    bool enabled{true}; ///< a press on it does something while its control takes one
    bool checked{};     ///< it is the half, level or item chosen
    std::string text;   ///< its caption, as shown
};

/// Returns the word a control names one of its parts by.
///
/// @param control the control
/// @param place the part's place, from 0
/// @return the control's word for it, else its place from 1
[[nodiscard]] std::string part_word(const Control& control, std::size_t place) {
    if (place < control.parts.size() && !control.parts[place].empty())
        return control.parts[place];
    return std::to_string(place + 1);
}

/// Returns the first item that draws a control in a role.
///
/// @param list the display list
/// @param id the control
/// @param role the role
/// @return the item; null when none draws it so
[[nodiscard]] const Item* drawn_as(const DisplayList& list, ControlId id, Role role) noexcept {
    for (const Item& item : list.items)
        if (item.control == id && item.role == role)
            return &item;
    return nullptr;
}

/// Returns a switch's halves: Off on the left, On on the right, as a press
/// on its right half from its middle column turns it on.
///
/// @param area the switch
/// @param look its state and captions
/// @return the two halves
[[nodiscard]] std::vector<Part> switch_halves(const Rect& area, const SwitchLook& look) {
    const int32_t half = area.width / 2;
    std::vector<Part> halves(2);
    halves[0] = {
        "off", {area.x, area.y, half, area.height}, true, true, !look.on, look.off_caption
    };
    halves[1] = {
        "on",
        {area.x + half, area.y, area.width - half, area.height},
        true,
        true,
        look.on,
        look.on_caption
    };
    return halves;
}

/// Returns a strip's levels: for each, the columns level_at finds it at.
///
/// @param control the strip's control, for its levels' words
/// @param area the strip
/// @param look its levels, their width, the one chosen and those offered
/// @return one part per level, left to right
[[nodiscard]] std::vector<Part>
strip_levels(const Control& control, const Rect& area, const LevelsLook& look) {
    const std::size_t count = look.captions.size();
    std::vector<Part> levels;
    for (std::size_t level = 0; level < count; ++level) {
        Part part;
        part.word = part_word(control, level);
        part.rect = {area.x, area.y, 0, area.height};
        part.enabled = level < look.offered;
        part.checked = level == look.chosen;
        part.text = look.captions[level];
        levels.push_back(std::move(part));
    }
    if (count == 0)
        return levels;
    // Each column belongs to the level a press there chooses.
    bool started = false;
    std::size_t current = 0;
    for (int32_t column = area.x; column < area.x + area.width; ++column) {
        const std::size_t level = level_at(area, count, look.level_width, column);
        Rect& rect = levels[level].rect;
        if (!started || level != current)
            rect.x = column;
        rect.width = column + 1 - rect.x;
        started = true;
        current = level;
    }
    return levels;
}

/// Returns an open drop-down's items as its menu shows them.
///
/// @param control the drop-down's control, for its items' words
/// @param list the display list, which holds the menu
/// @return one part per item the menu shows, top to bottom; none without a menu
[[nodiscard]] std::vector<Part> menu_items(const Control& control, const DisplayList& list) {
    std::vector<Part> items;
    for (const Item& item : list.items) {
        const auto* look = std::get_if<ChoiceMenuLook>(&item.look);
        if (item.role != Role::choice_menu || look == nullptr)
            continue;
        for (std::size_t place = 0; place < look->shown.size(); ++place) {
            const int32_t index = look->first + static_cast<int32_t>(place);
            const bool named = index >= 0 && static_cast<std::size_t>(index) < look->items.size();
            const ChoiceMenuRow* row =
                named ? &look->items[static_cast<std::size_t>(index)] : nullptr;
            // A divider is not a control and has no name.
            if (row != nullptr && row->divider)
                continue;
            Part part;
            if (row != nullptr && !row->id.empty())
                part.word = row->id;
            else
                part.word =
                    part_word(control, static_cast<std::size_t>(std::max(index, int32_t{0})));
            part.rect = menu_item_box(item.rect, *look, static_cast<int32_t>(place));
            part.inside = false;
            part.enabled = row == nullptr || row->choosable;
            part.checked = index == look->chosen;
            part.text = look->shown[place];
            items.push_back(std::move(part));
        }
        break;
    }
    return items;
}

/// Returns the parts of a control that take a press of their own.
///
/// @param list the display list
/// @param control the control
/// @return its parts, in order; none for a control of one press
[[nodiscard]] std::vector<Part> parts_of(const DisplayList& list, const Control& control) {
    if (control.kind == ControlKind::buttons) {
        std::vector<Part> buttons;
        for (const Item& item : list.items) {
            if (item.control != control.id || item.role != Role::button)
                continue;
            Part part;
            part.word = part_word(control, buttons.size());
            part.rect = item.rect;
            part.text = item.text;
            part.checked = item.state.selected;
            buttons.push_back(std::move(part));
        }
        return buttons;
    }
    if (const Item* drawn = drawn_as(list, control.id, Role::toggle))
        if (const auto* look = std::get_if<SwitchLook>(&drawn->look))
            return switch_halves(drawn->rect, *look);
    if (const Item* drawn = drawn_as(list, control.id, Role::levels))
        if (const auto* look = std::get_if<LevelsLook>(&drawn->look))
            return strip_levels(control, drawn->rect, *look);
    if (const Item* drawn = drawn_as(list, control.id, Role::choice))
        if (const auto* look = std::get_if<ChoiceLook>(&drawn->look); look != nullptr && look->open)
            return menu_items(control, list);
    return {};
}

/// Returns a named control as automation_entries lists it.
///
/// @param control the control
/// @param interaction the focus
/// @return the entry
[[nodiscard]] AutomationEntry entry_of(const Control& control, const Interaction& interaction) {
    AutomationEntry entry;
    entry.name = control.name;
    entry.kind = control.kind;
    entry.rect = control.rect;
    entry.shown = in_view(control);
    entry.enabled = control.enabled;
    entry.focused = interaction.focused != no_control && interaction.focused == control.id;
    entry.checked = control.checked;
    entry.text = control.text;
    entry.focusable = control.focusable;
    return entry;
}

/// Fills a slider's value from the look that draws it.
///
/// @param list the display list
/// @param control the slider
/// @param[in,out] entry its entry
void fill_slider(const DisplayList& list, const Control& control, AutomationEntry& entry) {
    for (const Item& item : list.items) {
        if (item.control != control.id)
            continue;
        const auto* look = std::get_if<SliderLook>(&item.look);
        if (look == nullptr)
            continue;
        entry.has_value = true;
        entry.has_range = true;
        entry.value = look->stop;
        entry.least = 0;
        entry.most = look->stops > 0 ? look->stops - 1 : 0;
        entry.step = 1;
        // The words the slider shows, such as "120 fps". The stop stays in value.
        entry.value_text = control.text;
        return;
    }
}

/// Fills the role, the label and the value a named control did not set itself.
///
/// @param list the display list, for a slider's look
/// @param control the control
/// @param[in,out] entry the entry entry_of returned
void describe_entry(const DisplayList& list, const Control& control, AutomationEntry& entry) {
    // A button, a link, a tab, a list row and an area say their text. A
    // switch, a strip, a drop-down, a slider, a field and a list are named
    // by the caption set on them, and the text is the value.
    const bool caption = control.kind == ControlKind::button || control.kind == ControlKind::link ||
                         control.kind == ControlKind::tab ||
                         control.kind == ControlKind::list_item ||
                         control.kind == ControlKind::area;
    if (!control.label.empty())
        entry.label = control.label;
    else if (caption)
        entry.label = control.text;
    entry.place = control.place;
    entry.set_size = control.set_size;
    if (control.search && control.kind == ControlKind::text_field) {
        entry.role = AccessRole::search;
        return;
    }
    switch (control.kind) {
    case ControlKind::button:
        entry.role = AccessRole::button;
        break;
    case ControlKind::choice:
        entry.role = AccessRole::button;
        entry.value_text = control.text;
        break;
    case ControlKind::toggle:
        entry.role = AccessRole::switch_control;
        entry.pressed = control.checked;
        entry.value_text = control.text;
        break;
    case ControlKind::levels:
        entry.role = AccessRole::radio_group;
        entry.value_text = control.text;
        break;
    case ControlKind::buttons:
        entry.role = AccessRole::radio_group;
        break;
    case ControlKind::tab:
        entry.role = AccessRole::tab;
        entry.chosen = control.checked;
        break;
    case ControlKind::list:
        entry.role = AccessRole::list;
        break;
    case ControlKind::list_item:
        entry.role = AccessRole::list_item;
        entry.chosen = control.checked;
        break;
    case ControlKind::text_field:
        entry.role = AccessRole::text_field;
        break;
    case ControlKind::slider:
    case ControlKind::scroll_bar:
        entry.role = AccessRole::slider;
        if (control.kind == ControlKind::slider)
            fill_slider(list, control, entry);
        break;
    case ControlKind::link:
        entry.role = AccessRole::link;
        break;
    case ControlKind::area:
        // An area that takes no press and no focus is words, such as a
        // library card's line. One a player can press is a button.
        entry.role = control.enabled || control.focusable ? AccessRole::button : AccessRole::label;
        break;
    }
    // A part of a composite reports its place in that group, as "2 of 9".
    if (const Composite* group = composite_of_part(list, control.id)) {
        const int32_t at = index_in(*group, control.id);
        if (at >= 0) {
            entry.place = at + 1;
            entry.set_size = static_cast<int32_t>(group->parts.size());
        }
    }
}

/// Fills the roles of a control's parts, and the group's place and value.
///
/// A switch's halves are check boxes. A strip's and a row's parts are radios.
/// An open menu's items are list items, and the menu's control stays a button.
///
/// @param[in,out] entry the control
/// @param[in,out] parts its parts, already built
void finish_group(AutomationEntry& entry, std::vector<AutomationEntry>& parts) {
    if (parts.empty())
        return;
    const auto count = static_cast<int32_t>(parts.size());
    if (entry.kind == ControlKind::toggle) {
        entry.role = AccessRole::switch_control;
        entry.pressed = entry.checked;
        for (AutomationEntry& part : parts) {
            part.role = AccessRole::check_box;
            part.chosen = part.checked;
            part.label = part.text;
        }
        return;
    }
    if (entry.kind == ControlKind::levels || entry.kind == ControlKind::buttons) {
        entry.role = AccessRole::radio_group;
        entry.set_size = count;
        int32_t place = 1;
        for (AutomationEntry& part : parts) {
            part.role = AccessRole::radio;
            part.chosen = part.checked;
            part.place = place;
            part.set_size = count;
            part.label = part.text;
            if (part.checked) {
                entry.place = place;
                entry.value_text = part.text;
            }
            ++place;
        }
        return;
    }
    if (entry.kind == ControlKind::choice) {
        for (AutomationEntry& part : parts) {
            part.role = AccessRole::list_item;
            part.chosen = part.checked;
            part.label = part.text;
        }
    }
}

/// Returns the nearest enabled control a finger on none reaches.
///
/// @param list the display list
/// @param finger the finger, which no control holds
/// @param within tells whether a squared distance, in points squared, is within reach
/// @return the control and its nearest point; no control and the finger when none is within reach
template <typename Within>
[[nodiscard]] Reached
nearest_control(const DisplayList& list, Point finger, Within within) noexcept {
    bool found = false;
    int64_t best_distance = 0;
    Reached best{no_control, finger};
    for (const Control& control : list.controls) {
        if (!control.enabled)
            continue;
        const Rect area = press_area(control);
        if (area.width <= 0 || area.height <= 0)
            continue;
        const Point candidate = nearest_point(area, finger);
        // A point another control covers is passed over: the press would land there.
        if (hit(list, candidate) != control.id)
            continue;
        const int64_t distance = distance_squared(candidate, finger);
        if (within(distance) && (!found || distance < best_distance)) {
            found = true;
            best_distance = distance;
            best = {control.id, candidate};
        }
    }
    return found ? best : Reached{no_control, finger};
}

} // namespace

ControlId composite_landing(const DisplayList& list, ControlId id) noexcept {
    const Composite* composite = composite_by_id(list, id);
    if (composite == nullptr)
        return id;
    const ControlId landed = landing_part(list, *composite);
    if (landed != no_control)
        composite->current = landed;
    return landed;
}

AxisMove move_along(const DisplayList& list, ControlId from, Direction direction) noexcept {
    const Composite* group = composite_by_id(list, from);
    if (group == nullptr)
        group = composite_of_part(list, from);
    if (group == nullptr)
        return {AxisStep::outside, no_control};

    const bool vertical = direction == Direction::up || direction == Direction::down;
    if (group->axis == CompositeAxis::column && !vertical)
        return {AxisStep::outside, no_control};
    if (group->axis == CompositeAxis::row && vertical)
        return {AxisStep::outside, no_control};

    const int32_t count = static_cast<int32_t>(group->parts.size());
    ControlId here = from;
    int32_t index = index_in(*group, from);
    if (index < 0) {
        here = landing_part(list, *group);
        index = index_in(*group, here);
    }
    if (index < 0)
        return {AxisStep::outside, no_control};

    if (group->axis != CompositeAxis::grid) {
        const int32_t step =
            (direction == Direction::down || direction == Direction::right) ? 1 : -1;
        for (int32_t at = index + step; at >= 0 && at < count; at += step) {
            const ControlId part = group->parts[static_cast<std::size_t>(at)];
            if (!part_live(list, part))
                continue;
            group->current = part;
            return {AxisStep::moved, part};
        }
        return {AxisStep::stayed, here};
    }

    const int32_t columns = group->columns < 1 ? int32_t{1} : group->columns;
    const int32_t column = index % columns;
    const int32_t row = index / columns;
    int32_t next = -1;
    if (direction == Direction::left) {
        if (column == 0)
            return {AxisStep::stayed, here};
        next = index - 1;
    } else if (direction == Direction::right) {
        if (column + 1 >= columns || index + 1 >= count)
            return {AxisStep::stayed, here};
        next = index + 1;
    } else if (direction == Direction::up) {
        if (row == 0)
            return {AxisStep::stayed, here};
        next = index - columns;
    } else {
        next = index + columns;
        if (next >= count)
            return {AxisStep::stayed, here};
    }
    if (next < 0 || next >= count || !part_live(list, group->parts[static_cast<std::size_t>(next)]))
        return {AxisStep::stayed, here};
    const ControlId part = group->parts[static_cast<std::size_t>(next)];
    group->current = part;
    return {AxisStep::moved, part};
}

ControlId composite_end(const DisplayList& list, ControlId from, bool last) noexcept {
    const Composite* group = composite_by_id(list, from);
    if (group == nullptr)
        group = composite_of_part(list, from);
    if (group == nullptr)
        return no_control;
    const int32_t count = static_cast<int32_t>(group->parts.size());
    ControlId found = no_control;
    if (!last) {
        for (int32_t index = 0; index < count; ++index) {
            const ControlId part = group->parts[static_cast<std::size_t>(index)];
            if (!part_live(list, part))
                continue;
            found = part;
            break;
        }
    } else {
        for (int32_t index = count - 1; index >= 0; --index) {
            const ControlId part = group->parts[static_cast<std::size_t>(index)];
            if (!part_live(list, part))
                continue;
            found = part;
            break;
        }
    }
    if (found == no_control) {
        if (index_in(*group, from) >= 0)
            return from;
        if (count == 0)
            return no_control;
        return last ? group->parts.back() : group->parts.front();
    }
    group->current = found;
    return found;
}

Reached reach(const DisplayList& list, Point finger, int32_t reach_px) noexcept {
    const ControlId under = hit(list, finger);
    if (under != no_control || reach_px <= 0)
        return {under, finger};
    const int64_t within = int64_t{reach_px} * reach_px;
    return nearest_control(list, finger, [within](int64_t distance) { return distance <= within; });
}

Reached reach(const DisplayList& list, Point finger, float within) noexcept {
    const ControlId under = hit(list, finger);
    if (under != no_control || !(within > 0.0F))
        return {under, finger};
    // A float's square is exact in double precision, and so is a squared
    // distance between two points of a canvas, so the distance is within the
    // reach exactly when its square is within the reach's square.
    const double limit = static_cast<double>(within) * static_cast<double>(within);
    return nearest_control(list, finger, [limit](int64_t distance) {
        return static_cast<double>(distance) <= limit;
    });
}

Reached
reach(const DisplayList& list, Point finger, int32_t reach_px, DisabledControl disabled) noexcept {
    if (disabled == DisabledControl::blocks)
        for (const Control& control : list.controls)
            if (contains(press_area(control), finger)) {
                if (!control.enabled)
                    return {no_control, finger};
                break;
            }
    return reach(list, finger, reach_px);
}

void mark_states(DisplayList& list, const Interaction& interaction) noexcept {
    for (Item& item : list.items) {
        if (item.control == no_control) {
            item.state.focused = false;
            item.state.pressed = false;
            continue;
        }
        item.state.focused = focus_ring_visible(interaction) && item.control == interaction.focused;
        item.state.pressed =
            interaction.pressed != no_control && item.control == interaction.pressed;
    }
}

PointerOutcome pointer_move(Interaction& interaction, const DisplayList& list, Point point) {
    if (interaction.pressed != no_control)
        point = shifted(point, interaction.finger_shift);
    const ControlId hovered = hit(list, point);
    // A move hides the ring, except on a text field: the field keeps its
    // ring, its caret and its typing. The focused control stays either way,
    // so the next key still acts on the control a click or a key placed.
    // An opening focus is left for the hidden-ring key table.
    const Control* focused = control_of(list, interaction.focused);
    const bool field = focused != nullptr && focused->kind == ControlKind::text_field;
    bool hid = false;
    if (field) {
        if (interaction.pointer_hid_ring) {
            interaction.pointer_hid_ring = false;
            hid = true;
        }
    } else if (interaction.focus_shown && !interaction.pointer_hid_ring) {
        interaction.pointer_hid_ring = true;
        hid = true;
    }
    if (hovered == interaction.hovered && !hid)
        return {PointerResult::none, hovered, point};
    interaction.hovered = hovered;
    return {PointerResult::redraw, hovered, point};
}

PointerOutcome pointer_down(Interaction& interaction, const DisplayList& list, Point point) {
    interaction.finger_shift = {};
    const ControlId control = hit(list, point);
    interaction.hovered = control;
    interaction.pressed = control;
    if (control == no_control)
        return {PointerResult::none, no_control, point};
    if (interaction.focus_shown && takes_focus(list, control)) {
        interaction.focused = control;
        remember_part(list, control);
    }
    return {PointerResult::redraw, control, point};
}

PointerOutcome
finger_down(Interaction& interaction, const DisplayList& list, Point finger, int32_t reach_px) {
    const Reached landed = reach(list, finger, reach_px);
    const PointerOutcome outcome = pointer_down(interaction, list, landed.at);
    if (interaction.pressed != no_control) {
        interaction.finger_shift = {landed.at.x - finger.x, landed.at.y - finger.y};
    }
    return outcome;
}

PointerOutcome pointer_up(Interaction& interaction, const DisplayList& list, Point point) {
    if (interaction.pressed != no_control)
        point = shifted(point, interaction.finger_shift);
    interaction.finger_shift = {};
    const ControlId hovered = hit(list, point);
    interaction.hovered = hovered;
    const ControlId held = interaction.pressed;
    interaction.pressed = no_control;
    // A completed press shows the ring again. A later move hides it.
    if (held != no_control)
        interaction.pointer_hid_ring = false;
    if (held == no_control)
        return {PointerResult::none, no_control, point};
    if (hovered != held)
        return {PointerResult::redraw, hovered, point};
    return {PointerResult::activated, held, point};
}

ControlId next_in_tab_order(const DisplayList& list, ControlId from, bool forward) noexcept {
    const std::vector<ControlId>& order = list.tab_order;
    if (order.empty())
        return no_control;
    const OrderPlace place = order_place(list, from);
    std::size_t index = 0;
    if (!place.found)
        index = forward ? 0 : order.size() - 1;
    else {
        const auto count = static_cast<std::ptrdiff_t>(order.size());
        const std::ptrdiff_t at = static_cast<std::ptrdiff_t>(place.index);
        const std::ptrdiff_t step = forward ? 1 : count - 1;
        index = static_cast<std::size_t>((at + step) % count);
    }
    return composite_landing(list, order[index]);
}

namespace {

/// Returns the control the focus moves to in a direction, as focus_toward says, judging
/// each control where `place` puts it.
///
/// @param list the display list
/// @param from the control the focus is on
/// @param direction the way it moves
/// @param place where a control is judged to lie, in points
/// @param whole true when every control is a candidate, whether its scroll area shows it or not
/// @return the control, or no_control when the focus stays
template <typename Place>
[[nodiscard]] ControlId focus_search(
    const DisplayList& list, ControlId from, Direction direction, Place place, bool whole
) noexcept {
    // How many gaps along the direction one gap across it costs, when nothing
    // lies in line with the focused control. The whole rule is this function,
    // so a playtest can tune it here.
    constexpr int32_t kAcrossWeight = 2;

    ControlId origin_id = from;
    if (const Composite* grouped = composite_by_id(list, from))
        origin_id = landing_part(list, *grouped);
    const Control* origin = control_of(list, origin_id);
    if (origin == nullptr)
        return no_control;
    const Rect from_rect = place(*origin);
    // A control in no scroll area has no area of its own to search first:
    // every control is outside it.
    const bool in_area = origin->group >= 0;

    const auto search = [&](bool same_group) noexcept -> ControlId {
        bool any = false;
        bool best_in_beam = false;
        int64_t best_rank = 0;
        int64_t best_centres = 0;
        std::size_t best_tab = 0;
        ControlId best = no_control;
        for (std::size_t index = 0; index < list.tab_order.size(); ++index) {
            const ControlId id = list.tab_order[index];
            ControlId candidate = id;
            Rect to_rect{};
            int32_t scroll_group = -1;
            bool visible = false;
            if (const Composite* grouped = composite_by_id(list, id)) {
                if (id == from || composite_of_part(list, from) == grouped)
                    continue;
                candidate = landing_part(list, *grouped);
                const Control* landed = control_of(list, candidate);
                if (landed == nullptr || !landed->focusable || !landed->enabled ||
                    candidate == from)
                    continue;
                if (!united_rect(list, *grouped, to_rect))
                    continue;
                scroll_group = landed->group;
                visible = any_in_view(list, *grouped);
            } else {
                if (id == from)
                    continue;
                const Control* control = control_of(list, id);
                if (control == nullptr || !control->focusable || !control->enabled)
                    continue;
                candidate = id;
                to_rect = place(*control);
                scroll_group = control->group;
                visible = in_view(*control);
            }
            if (same_group) {
                if (scroll_group != origin->group)
                    continue;
            } else if ((in_area && scroll_group == origin->group) || (!whole && !visible)) {
                continue;
            }
            if (!beyond(from_rect, to_rect, direction))
                continue;

            const bool in_beam =
                spans_overlap(across_span(from_rect, direction), across_span(to_rect, direction));
            const int64_t along = gap_along(from_rect, to_rect, direction);
            const int64_t across =
                span_gap(across_span(from_rect, direction), across_span(to_rect, direction));
            const int64_t rank =
                in_beam ? along : along + static_cast<int64_t>(kAcrossWeight) * across;
            const int64_t centres = centre_across(from_rect, to_rect, direction);

            bool better = !any;
            if (any) {
                if (in_beam != best_in_beam)
                    better = in_beam;
                else if (rank != best_rank)
                    better = rank < best_rank;
                else if (centres != best_centres)
                    better = centres < best_centres;
                else
                    better = index < best_tab;
            }
            if (!better)
                continue;
            any = true;
            best_in_beam = in_beam;
            best_rank = rank;
            best_centres = centres;
            best_tab = index;
            best = candidate;
        }
        return best;
    };

    if (in_area) {
        const ControlId inside = search(true);
        if (inside != no_control)
            return inside;
    }
    return search(false);
}

} // namespace

ControlId focus_toward(const DisplayList& list, ControlId from, Direction direction) noexcept {
    return focus_search(
        list, from, direction, [](const Control& control) noexcept { return control.rect; }, false
    );
}

ControlId focus_toward(
    const DisplayList& list, ControlId from, Direction direction, std::span<const ScrollArea> areas
) noexcept {
    const auto laid_whole = [areas](const Control& control) noexcept {
        Rect rect = control.rect;
        if (control.group >= 0 && static_cast<std::size_t>(control.group) < areas.size()) {
            rect.y += areas[static_cast<std::size_t>(control.group)].offset;
            return rect;
        }
        for (const ScrollArea& area : areas)
            if (area.view.height > 0 && control.rect.y >= area.view.y + area.view.height)
                rect.y += area.limit;
        return rect;
    };
    return focus_search(list, from, direction, laid_whole, true);
}

namespace {

/// Builds what a key did.
///
/// @param result what the screen does
/// @param control the control it names
/// @param pressed the key
/// @return the outcome
[[nodiscard]] KeyOutcome outcome_of(KeyResult result, ControlId control, Key pressed) noexcept {
    return {result, control, pressed};
}

/// Shows the focus on one end of the declared order.
///
/// @param[in,out] interaction the focus
/// @param list the display list
/// @param last true for the last control, false for the first
/// @param pressed the key
/// @return redraw, or none when the order is empty
[[nodiscard]] KeyOutcome
show_end(Interaction& interaction, const DisplayList& list, bool last, Key pressed) {
    if (list.tab_order.empty())
        return outcome_of(KeyResult::none, no_control, pressed);
    interaction.focused =
        composite_landing(list, last ? list.tab_order.back() : list.tab_order.front());
    interaction.focus_shown = true;
    interaction.pointer_hid_ring = false;
    return outcome_of(KeyResult::redraw, interaction.focused, pressed);
}

/// Shows the ring on the control Tab or Shift+Tab reaches while the ring is hidden.
///
/// From a control in the order, Tab moves on and Shift+Tab moves back, and
/// the ring is shown there. From none, or from a control that is not in the
/// order, Tab shows the first and Shift+Tab the last, as they do today.
///
/// @param[in,out] interaction the focus
/// @param list the display list
/// @param forward true for Tab, false for Shift+Tab
/// @param pressed the key
/// @return redraw, or none when the order is empty
[[nodiscard]] KeyOutcome
show_hidden_tab(Interaction& interaction, const DisplayList& list, bool forward, Key pressed) {
    if (list.tab_order.empty())
        return outcome_of(KeyResult::none, no_control, pressed);
    // A part of a composite is in the order, on that one stop. From a control
    // that is not, Tab shows the first stop and Shift+Tab the last.
    if (!order_place(list, interaction.focused).found)
        return show_end(interaction, list, !forward, pressed);
    interaction.focused = next_in_tab_order(list, interaction.focused, forward);
    interaction.focus_shown = true;
    interaction.pointer_hid_ring = false;
    return outcome_of(KeyResult::redraw, interaction.focused, pressed);
}

} // namespace

KeyOutcome key(Interaction& interaction, const DisplayList& list, Key pressed) {
    // A key shows the ring again where a click or a key left the focus. Escape
    // and the screen's own Yes, No and Info leave a pointer-hidden ring hidden.
    // An opening focus is unchanged, so the hidden-ring table still applies.
    if (pressed != Key::escape && pressed != Key::yes && pressed != Key::no && pressed != Key::info)
        show_ring_for_key(interaction);
    const bool shown = interaction.focus_shown && interaction.focused != no_control;
    const Control* focused = shown ? control_of(list, interaction.focused) : nullptr;

    const bool on_field = focused != nullptr && focused->kind == ControlKind::text_field;
    switch (pressed) {
    case Key::escape:
        return outcome_of(KeyResult::cancel, no_control, pressed);
    case Key::yes:
    case Key::no:
    case Key::info:
        return outcome_of(KeyResult::none, no_control, pressed);
    case Key::backspace:
    case Key::delete_forward:
        if (on_field)
            return outcome_of(KeyResult::to_control, interaction.focused, pressed);
        return outcome_of(KeyResult::none, no_control, pressed);
    case Key::home:
    case Key::end:
        if (on_field)
            return outcome_of(KeyResult::to_control, interaction.focused, pressed);
        if (shown) {
            const ControlId end_part =
                composite_end(list, interaction.focused, pressed == Key::end);
            if (end_part != no_control) {
                if (end_part == interaction.focused)
                    return outcome_of(KeyResult::none, interaction.focused, pressed);
                interaction.focused = end_part;
                interaction.focus_shown = true;
                interaction.pointer_hid_ring = false;
                return outcome_of(KeyResult::redraw, end_part, pressed);
            }
        }
        return outcome_of(KeyResult::scroll, shown ? interaction.focused : no_control, pressed);
    case Key::page_up:
    case Key::page_down:
        return outcome_of(KeyResult::scroll, shown ? interaction.focused : no_control, pressed);
    case Key::enter:
        if (focused != nullptr &&
            (focused->kind == ControlKind::button || focused->kind == ControlKind::link ||
             focused->kind == ControlKind::list_item || focused->kind == ControlKind::tab ||
             composite_of_part(list, focused->id) != nullptr)) {
            // The press stands, and the ring goes, so the next keys are the
            // screen's until a key shows the ring again.
            interaction.focus_shown = false;
            return outcome_of(KeyResult::to_control, interaction.focused, pressed);
        }
        // A text field keeps its ring, its caret and its typing, as it does
        // through a pointer move: the screen accepts and the field still edits.
        if (shown && (focused == nullptr || focused->kind != ControlKind::text_field))
            interaction.focus_shown = false;
        return outcome_of(KeyResult::accept, no_control, pressed);
    case Key::up:
        if (!shown)
            return show_end(interaction, list, true, pressed);
        break;
    case Key::back_tab:
        if (!shown)
            return show_hidden_tab(interaction, list, false, pressed);
        break;
    case Key::tab:
        if (!shown)
            return show_hidden_tab(interaction, list, true, pressed);
        break;
    case Key::down:
    case Key::left:
    case Key::right:
    case Key::space:
        if (!shown)
            return show_end(interaction, list, false, pressed);
        break;
    }

    if (pressed == Key::space)
        return outcome_of(KeyResult::to_control, interaction.focused, pressed);

    if ((pressed == Key::left || pressed == Key::right) && focused != nullptr && focused->steps)
        return outcome_of(KeyResult::to_control, interaction.focused, pressed);

    if (pressed == Key::tab || pressed == Key::back_tab) {
        const ControlId next = next_in_tab_order(list, interaction.focused, pressed == Key::tab);
        if (next == no_control || next == interaction.focused)
            return outcome_of(KeyResult::none, interaction.focused, pressed);
        interaction.focused = next;
        interaction.focus_shown = true;
        interaction.pointer_hid_ring = false;
        return outcome_of(KeyResult::redraw, next, pressed);
    }

    Direction direction = Direction::up;
    if (pressed == Key::down)
        direction = Direction::down;
    else if (pressed == Key::left)
        direction = Direction::left;
    else if (pressed == Key::right)
        direction = Direction::right;
    else if (pressed != Key::up)
        return outcome_of(KeyResult::none, no_control, pressed);

    const AxisMove along = move_along(list, interaction.focused, direction);
    if (along.step == AxisStep::stayed)
        return outcome_of(KeyResult::none, interaction.focused, pressed);
    if (along.step == AxisStep::moved) {
        interaction.focused = along.part;
        interaction.focus_shown = true;
        interaction.pointer_hid_ring = false;
        return outcome_of(KeyResult::redraw, along.part, pressed);
    }

    const ControlId next = focus_toward(list, interaction.focused, direction);
    if (next == no_control)
        return outcome_of(KeyResult::none, interaction.focused, pressed);
    interaction.focused = next;
    interaction.focus_shown = true;
    interaction.pointer_hid_ring = false;
    remember_part(list, next);
    return outcome_of(KeyResult::redraw, next, pressed);
}

void focus_opened(Interaction& interaction, const DisplayList& list, bool show) noexcept {
    if (interaction.focused != no_control)
        return;
    if (list.tab_order.empty())
        return;
    interaction.focused = composite_landing(list, list.tab_order.front());
    interaction.focus_shown = show;
    if (show)
        interaction.pointer_hid_ring = false;
}

namespace {

/// The first character that is not a control character.
constexpr char32_t kFirstPrintable = 0x20;
/// DEL, the control character at the end of ASCII.
constexpr char32_t kDeleteCharacter = 0x7f;
/// The first byte of a sequence of more than one byte, and the first
/// character such a sequence holds.
constexpr unsigned char kFirstSequenceByte = 0x80;
/// The control characters after ASCII, U+0080 to U+009F.
constexpr char32_t kFirstLatinControl = 0x80;
constexpr char32_t kLastLatinControl = 0x9f;

/// Tells whether a character is a control character a field refuses.
///
/// @param character the character
/// @return true for U+0000 to U+001F, U+007F and U+0080 to U+009F
[[nodiscard]] bool control_character(char32_t character) noexcept {
    return character < kFirstPrintable || character == kDeleteCharacter ||
           (character >= kFirstLatinControl && character <= kLastLatinControl);
}

/// Tells whether a text is well-formed UTF-8 without a control character.
///
/// @param text the text
/// @return true when every character is well formed and none is a control character
[[nodiscard]] bool typeable(std::string_view text) noexcept {
    for (std::size_t at = 0; at < text.size();) {
        const auto first = static_cast<unsigned char>(text[at]);
        if (first < kFirstSequenceByte) {
            if (control_character(first))
                return false;
            ++at;
            continue;
        }
        const oa::present::TextCharacter character = oa::present::utf8_sequence(text.substr(at));
        if (!character.utf8_sequence || character.bytes == 0 ||
            control_character(character.code_point))
            return false;
        at += character.bytes;
    }
    return true;
}

/// Moves a field's caret back onto the text: to its end when past it, and
/// otherwise to the start of the character it is inside.
///
/// @param[in,out] field the text and its caret
void settle_caret(TextField& field) noexcept {
    field.caret = std::min(field.caret, field.text.size());
    while (field.caret > 0 && field.caret < field.text.size() &&
           continuation_byte(field.text[field.caret]))
        --field.caret;
}

/// Returns the start of the character before a byte.
///
/// @param text the text
/// @param at a character boundary, above 0
/// @return the previous character's first byte
[[nodiscard]] std::size_t previous_boundary(std::string_view text, std::size_t at) noexcept {
    std::size_t before = at - 1;
    while (before > 0 && continuation_byte(text[before]))
        --before;
    return before;
}

/// Returns the boundary after the character at a byte.
///
/// @param text the text
/// @param at a character boundary before the text's end
/// @return the next character's first byte, or the text's end
[[nodiscard]] std::size_t next_boundary(std::string_view text, std::size_t at) noexcept {
    std::size_t after = at + 1;
    while (after < text.size() && continuation_byte(text[after]))
        ++after;
    return after;
}

} // namespace

bool insert_text(TextField& field, std::string_view utf8) {
    if (utf8.empty() || !typeable(utf8))
        return false;
    settle_caret(field);
    field.text.insert(field.caret, utf8);
    field.caret += utf8.size();
    return true;
}

bool edit_text(TextField& field, Key pressed) {
    const std::size_t was = field.caret;
    settle_caret(field);
    const std::string_view text = field.text;
    switch (pressed) {
    case Key::backspace:
        if (field.caret > 0) {
            const std::size_t start = previous_boundary(text, field.caret);
            field.text.erase(start, field.caret - start);
            field.caret = start;
            return true;
        }
        break;
    case Key::delete_forward:
        if (field.caret < field.text.size()) {
            const std::size_t end = next_boundary(text, field.caret);
            field.text.erase(field.caret, end - field.caret);
            return true;
        }
        break;
    case Key::left:
        if (field.caret > 0)
            field.caret = previous_boundary(text, field.caret);
        break;
    case Key::right:
        if (field.caret < field.text.size())
            field.caret = next_boundary(text, field.caret);
        break;
    case Key::home:
        field.caret = 0;
        break;
    case Key::end:
        field.caret = field.text.size();
        break;
    default:
        break;
    }
    return field.caret != was;
}

int32_t wheel_offset(
    WheelCarry& carry, float notches, int32_t step, int32_t offset, int32_t limit
) noexcept {
    if (!std::isfinite(notches))
        return offset;
    if (limit == 0) {
        carry.rows = 0.0F;
        return offset;
    }
    // A turn larger than the area reaches its end. The extra point lets a
    // fraction past the last row still arrive there.
    const float room = static_cast<float>(limit) + 1.0F;
    const float rows = std::clamp(carry.rows - notches * static_cast<float>(step), -room, room);
    const auto whole = static_cast<int32_t>(rows);
    carry.rows = rows - static_cast<float>(whole);
    const int32_t next = std::clamp(offset + whole, int32_t{0}, limit);
    // What is carried towards an end already reached is dropped.
    if ((next == 0 && carry.rows < 0.0F) || (next == limit && carry.rows > 0.0F))
        carry.rows = 0.0F;
    return next;
}

namespace {

/// Appends one entry for each named composite.
///
/// The group is a list, with the role its CompositeRole names. Its place is
/// the remembered part's, and it is focused while the focus is on a part.
///
/// @param list the display list
/// @param interaction the focus
/// @param[in,out] entries the entries so far
void append_composites(
    const DisplayList& list, const Interaction& interaction, std::vector<AutomationEntry>& entries
) {
    for (const Composite& composite : list.composites) {
        if (composite.name.empty())
            continue;
        AutomationEntry entry;
        entry.name = composite.name;
        entry.kind = ControlKind::list;
        entry.role =
            composite.role == CompositeRole::tab_list ? AccessRole::tab_list : AccessRole::list;
        entry.focusable = true;
        entry.set_size = static_cast<int32_t>(composite.parts.size());
        Rect united{};
        if (united_rect(list, composite, united))
            entry.rect = united;
        entry.shown = any_in_view(list, composite);
        bool enabled = false;
        for (const ControlId part : composite.parts) {
            const Control* control = control_of(list, part);
            if (control != nullptr && control->enabled)
                enabled = true;
            if (part == interaction.focused)
                entry.focused = true;
        }
        if (interaction.focused == composite.id)
            entry.focused = true;
        entry.enabled = enabled;
        const ControlId landed = landing_part(list, composite);
        const int32_t at = index_in(composite, landed);
        if (at >= 0)
            entry.place = at + 1;
        const Control* chosen = control_of(list, landed);
        if (chosen != nullptr && chosen->checked)
            entry.chosen = true;
        entries.push_back(std::move(entry));
    }
}

} // namespace

std::vector<AutomationEntry>
automation_entries(const DisplayList& list, const Interaction& interaction) {
    std::vector<AutomationEntry> entries;
    for (const Control& control : list.controls)
        if (!control.name.empty()) {
            AutomationEntry entry = entry_of(control, interaction);
            describe_entry(list, control, entry);
            entries.push_back(std::move(entry));
        }
    append_composites(list, interaction, entries);
    return entries;
}

std::vector<AutomationEntry>
automation_parts(const DisplayList& list, const Interaction& interaction) {
    // Each named control with its parts, before a control an item stands for
    // is left out.
    std::vector<std::pair<AutomationEntry, std::vector<AutomationEntry>>> listed;
    std::vector<std::string> part_names;
    for (const Control& control : list.controls) {
        if (control.name.empty())
            continue;
        AutomationEntry entry = entry_of(control, interaction);
        describe_entry(list, control, entry);
        const Rect area = press_area(control);
        if (has_room(area))
            entry.rect = area;
        std::vector<AutomationEntry> parts;
        for (Part& part : parts_of(list, control)) {
            AutomationEntry added;
            added.name = control.name + "." + part.word;
            added.kind = control.kind;
            added.rect = part.rect;
            added.shown = has_room(part.rect);
            if (part.inside) {
                // A part shows, and is pressed, where its control's press area holds it.
                const Rect reached = intersect(part.rect, area);
                added.shown = has_room(reached);
                if (added.shown)
                    added.rect = reached;
            }
            added.enabled = control.enabled && part.enabled;
            added.checked = part.checked;
            added.text = std::move(part.text);
            added.part = true;
            part_names.push_back(added.name);
            parts.push_back(std::move(added));
        }
        finish_group(entry, parts);
        listed.emplace_back(std::move(entry), std::move(parts));
    }
    std::vector<AutomationEntry> entries;
    for (auto& [entry, parts] : listed) {
        // A control the screen made of an open drop-down's item is that item.
        if (std::find(part_names.begin(), part_names.end(), entry.name) == part_names.end())
            entries.push_back(std::move(entry));
        for (AutomationEntry& part : parts)
            entries.push_back(std::move(part));
    }
    append_composites(list, interaction, entries);
    return entries;
}

ControlId control_named(const DisplayList& list, std::string_view name) noexcept {
    for (const Control& control : list.controls)
        if (std::string_view{control.name} == name)
            return control.id;
    return no_control;
}

std::string name_problem(const DisplayList& list) {
    std::vector<std::string_view> seen;
    seen.reserve(list.controls.size());
    for (const Control& control : list.controls) {
        const std::string& control_name = control.name;
        if (control_name.empty())
            return "Control " + std::to_string(control.id) + " has no name.";
        if (control_name.size() > 100)
            return "The name of control " + std::to_string(control.id) + " is " +
                   std::to_string(control_name.size()) + " bytes, and a name is at most 100.";
        if (!name_words(control_name))
            return "The name \"" + control_name +
                   "\" is not words of a-z, 0-9 and hyphens joined by dots.";
        for (const std::string_view earlier : seen) {
            if (earlier == control_name)
                return "The name \"" + control_name + "\" is used more than once.";
        }
        seen.push_back(control_name);
    }
    for (const Composite& composite : list.composites) {
        const std::string& group_name = composite.name;
        if (group_name.empty())
            continue;
        if (group_name.size() > 100)
            return "The name of control " + std::to_string(composite.id) + " is " +
                   std::to_string(group_name.size()) + " bytes, and a name is at most 100.";
        if (!name_words(group_name))
            return "The name \"" + group_name +
                   "\" is not words of a-z, 0-9 and hyphens joined by dots.";
        for (const std::string_view earlier : seen)
            if (earlier == group_name)
                return "The name \"" + group_name + "\" is used more than once.";
        seen.push_back(group_name);
    }
    return {};
}

} // namespace oa::ui::kit
