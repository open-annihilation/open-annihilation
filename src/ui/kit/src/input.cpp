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

/// Tells whether the focus may stop on a control.
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
    entry.focused = interaction.focus_shown && interaction.focused == control.id;
    entry.checked = control.checked;
    entry.text = control.text;
    entry.focusable = control.focusable;
    return entry;
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
        item.state.focused = interaction.focus_shown && item.control == interaction.focused;
        item.state.pressed =
            interaction.pressed != no_control && item.control == interaction.pressed;
    }
}

PointerOutcome pointer_move(Interaction& interaction, const DisplayList& list, Point point) {
    if (interaction.pressed != no_control)
        point = shifted(point, interaction.finger_shift);
    const ControlId hovered = hit(list, point);
    if (hovered == interaction.hovered)
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
    if (interaction.focus_shown && takes_focus(list, control))
        interaction.focused = control;
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
    const auto found = std::find(order.begin(), order.end(), from);
    if (found == order.end())
        return forward ? order.front() : order.back();
    const auto count = static_cast<std::ptrdiff_t>(order.size());
    const std::ptrdiff_t at = found - order.begin();
    const std::ptrdiff_t step = forward ? 1 : count - 1;
    return order[static_cast<std::size_t>((at + step) % count)];
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

    const Control* origin = control_of(list, from);
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
            if (id == from)
                continue;
            const Control* control = control_of(list, id);
            if (control == nullptr || !control->focusable || !control->enabled)
                continue;
            if (same_group) {
                if (control->group != origin->group)
                    continue;
            } else if (
                (in_area && control->group == origin->group) || (!whole && !in_view(*control))
            ) {
                continue;
            }
            const Rect to_rect = place(*control);
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
            best = id;
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
    interaction.focused = last ? list.tab_order.back() : list.tab_order.front();
    interaction.focus_shown = true;
    return outcome_of(KeyResult::redraw, interaction.focused, pressed);
}

} // namespace

KeyOutcome key(Interaction& interaction, const DisplayList& list, Key pressed) {
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
        return outcome_of(KeyResult::scroll, shown ? interaction.focused : no_control, pressed);
    case Key::page_up:
    case Key::page_down:
        return outcome_of(KeyResult::scroll, shown ? interaction.focused : no_control, pressed);
    case Key::enter:
        if (focused != nullptr &&
            (focused->kind == ControlKind::button || focused->kind == ControlKind::link ||
             focused->kind == ControlKind::list_item || focused->kind == ControlKind::tab))
            return outcome_of(KeyResult::to_control, interaction.focused, pressed);
        return outcome_of(KeyResult::accept, no_control, pressed);
    case Key::up:
    case Key::back_tab:
        if (!shown)
            return show_end(interaction, list, true, pressed);
        break;
    case Key::tab:
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

    const ControlId next = focus_toward(list, interaction.focused, direction);
    if (next == no_control)
        return outcome_of(KeyResult::none, interaction.focused, pressed);
    interaction.focused = next;
    interaction.focus_shown = true;
    return outcome_of(KeyResult::redraw, next, pressed);
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

std::vector<AutomationEntry>
automation_entries(const DisplayList& list, const Interaction& interaction) {
    std::vector<AutomationEntry> entries;
    for (const Control& control : list.controls)
        if (!control.name.empty())
            entries.push_back(entry_of(control, interaction));
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
    return {};
}

} // namespace oa::ui::kit
