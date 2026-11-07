// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The chooser's hit test, presses, keys, focus and scrolling
// (folder_chooser.hpp), over the kit's display list. A press reaches the
// target the kit's reach finds, a disabled one holding it to no target; the
// arrows move the focus by the kit's two-dimensional focus, judged with the
// rows laid out whole; Tab moves through the focus order, and a target the
// focus reaches is scrolled into view.
#include "oa/ui/folder_chooser.hpp"

#include "folder_chooser_internal.hpp"

#include "oa/ui/kit/input.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <string>

namespace oa::ui::folder_chooser {

namespace detail {

const oa::ui::kit::Item* target_part(const oa::ui::kit::DisplayList& list, int32_t place) noexcept {
    if (place < 0)
        return nullptr;
    int32_t seen = 0;
    for (const oa::ui::kit::Item& part : list.items) {
        if (part.control == oa::ui::kit::no_control)
            continue;
        if (seen == place)
            return &part;
        ++seen;
    }
    return nullptr;
}

} // namespace detail

namespace {

namespace kit = oa::ui::kit;

/// The bits under a target's kind in its control number: its index.
constexpr int index_bits = 16;
/// The bits of a control number that hold the target's index.
constexpr kit::ControlId index_mask = 0xffff;

/// Returns how far a press reaches beyond a part: pick_reach_points at the layout's density.
///
/// @param layout the layout
/// @return canvas pixels
int press_reach(const Layout& layout) noexcept {
    const float scale = layout.paint.px_per_point > 0.0f ? layout.paint.px_per_point : 1.0f;
    return static_cast<int>(std::lround(oa::ui::game_files::pick_reach_points * scale));
}

/// Finds the part that draws a target.
///
/// @param layout the layout
/// @param target the target
/// @return the part; null when the target is not laid out
const kit::Item* part_of(const Layout& layout, Target target) noexcept {
    const kit::ControlId id = control_id(target);
    if (id == kit::no_control)
        return nullptr;
    for (const kit::Item& part : layout.paint.list.items)
        if (part.control == id)
            return &part;
    return nullptr;
}

/// Finds a target's place in the order the targets were laid out.
///
/// @param layout the layout
/// @param target the target
/// @return its place; none when it is not laid out
std::optional<int32_t> place_of(const Layout& layout, Target target) noexcept {
    const kit::ControlId id = control_id(target);
    int32_t seen = 0;
    for (const kit::Item& part : layout.paint.list.items) {
        if (part.control == kit::no_control)
            continue;
        if (part.control == id)
            return seen;
        ++seen;
    }
    return std::nullopt;
}

/// Returns what pressing a target asks of the app.
///
/// @param target the target
/// @return its command
Outcome outcome_of(Target target) noexcept {
    switch (target.kind) {
    case TargetKind::none:
        break;
    case TargetKind::place:
        return {Command::open_place, target.index};
    case TargetKind::up:
        return {Command::parent_folder, 0};
    case TargetKind::filter:
        return {Command::toggle_filter, 0};
    case TargetKind::entry:
        return {Command::activate_entry, target.index};
    case TargetKind::dialog:
        return {Command::use_dialog, 0};
    case TargetKind::cancel:
        return {Command::cancel, 0};
    case TargetKind::choose:
        return {Command::choose, 0};
    }
    return {};
}

/// Scrolls the rows so that a target's part shows whole in its scrolling region, as far as
/// the layout's range allows.
///
/// @param layout the layout
/// @param[in,out] state what the chooser keeps between frames
/// @param target the target
void scroll_into_view(const Layout& layout, UiState& state, Target target) noexcept {
    const kit::Item* part = part_of(layout, target);
    if (part == nullptr || part->clip.height <= 0)
        return;
    const Rect& box = part->rect;
    const Rect& clip = part->clip;
    const float scale = layout.paint.px_per_point > 0.0f ? layout.paint.px_per_point : 1.0f;
    int32_t points = 0;
    if (box.y < clip.y)
        points = -static_cast<int32_t>(std::ceil(static_cast<float>(clip.y - box.y) / scale));
    else if (box.y + box.height > clip.y + clip.height)
        points = static_cast<int32_t>(
            std::ceil(static_cast<float>(box.y + box.height - (clip.y + clip.height)) / scale)
        );
    if (points != 0)
        scroll(layout, state, points);
}

/// Returns the rows' scroll area as the arrows judge it: the scrolling region, scrolled as far
/// as the chooser keeps it, and the layout's range, in canvas pixels.
///
/// @param layout the layout
/// @param state the scroll the chooser keeps
/// @return the scroll area; an empty view when nothing scrolls
kit::ScrollArea rows_area(const Layout& layout, const UiState& state) noexcept {
    const float scale = layout.paint.px_per_point > 0.0f ? layout.paint.px_per_point : 1.0f;
    const int32_t most = std::max(0, layout.scroll_max_points);
    kit::ScrollArea area{};
    area.view = layout.paint.rows;
    area.offset = static_cast<int32_t>(
        std::lround(static_cast<float>(std::clamp(state.scroll_points, 0, most)) * scale)
    );
    area.limit = static_cast<int32_t>(std::lround(static_cast<float>(most) * scale));
    return area;
}

/// Picks the target of the focus order the arrows move the focus to from the focused one, by
/// the kit's two-dimensional focus with the rows laid out whole: rows scrolled out of view lie
/// between the fixed parts as they do in the column.
///
/// @param layout the layout
/// @param state the scroll it was laid out with
/// @param from the focused entry of the focus order
/// @param direction the way the focus moves
/// @return the entry of the focus order; none when nothing lies that way
std::optional<int32_t> arrow_target(
    const Layout& layout, const UiState& state, int32_t from, kit::Direction direction
) noexcept {
    const kit::ScrollArea rows = rows_area(layout, state);
    const kit::ControlId next = kit::focus_toward(
        layout.paint.list,
        control_id(layout.focus_order[static_cast<std::size_t>(from)]),
        direction,
        std::span<const kit::ScrollArea>(&rows, 1)
    );
    if (next == kit::no_control)
        return std::nullopt;
    const Target target = target_of(next);
    for (std::size_t index = 0; index < layout.focus_order.size(); ++index)
        if (layout.focus_order[index] == target)
            return static_cast<int32_t>(index);
    return std::nullopt;
}

} // namespace

kit::ControlId control_id(Target target) noexcept {
    if (target.kind == TargetKind::none)
        return kit::no_control;
    return (static_cast<kit::ControlId>(target.kind) << index_bits) |
           static_cast<kit::ControlId>(target.index);
}

Target target_of(kit::ControlId id) noexcept {
    if (id <= 0)
        return {};
    const kit::ControlId kind = id >> index_bits;
    if (kind <= static_cast<kit::ControlId>(TargetKind::none) ||
        kind > static_cast<kit::ControlId>(TargetKind::choose))
        return {};
    return {static_cast<TargetKind>(kind), static_cast<uint16_t>(id & index_mask)};
}

Target hit_test(const Layout& layout, Point point, int reach_px) noexcept {
    // A target that holds the point takes it; a disabled one holds it to no target.
    return target_of(
        kit::reach(layout.paint.list, point, reach_px, kit::DisabledControl::blocks).control
    );
}

void press_down(const Layout& layout, UiState& state, Point point) noexcept {
    state.pressed = -1;
    state.focus_shown = false;
    const Target target = hit_test(layout, point, press_reach(layout));
    if (target.kind == TargetKind::none)
        return;
    if (const auto place = place_of(layout, target))
        state.pressed = *place;
}

Outcome press_up(const Layout& layout, UiState& state, Point point) noexcept {
    const int32_t pressed = state.pressed;
    state.pressed = -1;
    const kit::Item* held = detail::target_part(layout.paint.list, pressed);
    if (held == nullptr)
        return {};
    const Target target = target_of(held->control);
    if (held->state.disabled || hit_test(layout, point, press_reach(layout)) != target)
        return {};
    // The focus follows the press, for keys or a pad that come next.
    for (std::size_t index = 0; index < layout.focus_order.size(); ++index)
        if (layout.focus_order[index] == target)
            state.focus = static_cast<int32_t>(index);
    return outcome_of(target);
}

/// Returns select_entry when a target is a row, so the side panel follows the focus.
///
/// @param target the target the focus reached
/// @return the selection, or none
Outcome selection_of(Target target) noexcept {
    if (target.kind != TargetKind::entry)
        return {};
    return {Command::select_entry, target.index};
}

Outcome key(const Layout& layout, UiState& state, Key pressed) noexcept {
    const int32_t count = static_cast<int32_t>(layout.focus_order.size());
    if (pressed == Key::choose) {
        const kit::Item* button = part_of(layout, Target{TargetKind::choose, 0});
        if (button != nullptr && !button->state.disabled)
            return {Command::choose, 0};
        return {};
    }
    if (pressed == Key::filter)
        return {Command::toggle_filter, 0};
    if (pressed == Key::escape) {
        const kit::Item* up = part_of(layout, Target{TargetKind::up, 0});
        if (up != nullptr && !up->state.disabled)
            return {Command::parent_folder, 0};
        return {Command::cancel, 0};
    }
    if (pressed == Key::page_up || pressed == Key::page_down) {
        const float scale = layout.paint.px_per_point > 0.0f ? layout.paint.px_per_point : 1.0f;
        const int32_t page = std::max<int32_t>(
            1,
            static_cast<int32_t>(std::lround(static_cast<float>(layout.paint.rows.height) / scale))
        );
        scroll(layout, state, pressed == Key::page_down ? page : -page);
        return {};
    }
    if (count == 0)
        return {};
    // Out of the order: Tab shows the first target, Shift+Tab the last, and
    // every other key the first. A valid index with the ring hidden moves on
    // for Tab and Shift+Tab, and shows the ring where it is for every other
    // key. Enter does not press until the ring is showing.
    if (!state.focus_shown || state.focus < 0 || state.focus >= count) {
        const bool out_of_range = state.focus < 0 || state.focus >= count;
        if (out_of_range)
            state.focus = pressed == Key::back_tab ? count - 1 : 0;
        else if (pressed == Key::tab)
            state.focus = (state.focus + 1) % count;
        else if (pressed == Key::back_tab)
            state.focus = (state.focus + count - 1) % count;
        state.focus_shown = true;
        const Target shown = layout.focus_order[static_cast<std::size_t>(state.focus)];
        scroll_into_view(layout, state, shown);
        return selection_of(shown);
    }
    std::optional<int32_t> next;
    switch (pressed) {
    case Key::tab:
        next = (state.focus + 1) % count;
        break;
    case Key::back_tab:
        next = (state.focus + count - 1) % count;
        break;
    case Key::up:
        next = arrow_target(layout, state, state.focus, kit::Direction::up);
        break;
    case Key::down:
        next = arrow_target(layout, state, state.focus, kit::Direction::down);
        break;
    case Key::left:
        next = arrow_target(layout, state, state.focus, kit::Direction::left);
        break;
    case Key::right:
        next = arrow_target(layout, state, state.focus, kit::Direction::right);
        break;
    case Key::enter: {
        const Outcome pressed_row =
            outcome_of(layout.focus_order[static_cast<std::size_t>(state.focus)]);
        state.focus_shown = false;
        return pressed_row;
    }
    case Key::escape:
    case Key::page_up:
    case Key::page_down:
    case Key::choose:
    case Key::filter:
        break;
    }
    if (next) {
        state.focus = *next;
        const Target shown = layout.focus_order[static_cast<std::size_t>(state.focus)];
        scroll_into_view(layout, state, shown);
        return selection_of(shown);
    }
    return {};
}

namespace {

/// Returns a name with A–Z folded to a–z. Every other byte stays as it is.
///
/// @param name the name
/// @return the folded name
std::string folded_ascii(std::string_view name) {
    std::string folded(name);
    for (char& character : folded) {
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character - 'A' + 'a');
    }
    return folded;
}

} // namespace

bool entry_before(const Entry& a, const Entry& b) {
    if (a.directory != b.directory)
        return a.directory;
    const std::string folded_a = folded_ascii(a.name);
    const std::string folded_b = folded_ascii(b.name);
    if (folded_a != folded_b)
        return folded_a < folded_b;
    return a.name < b.name;
}

void order_entries(std::vector<Entry>& entries) {
    std::stable_sort(entries.begin(), entries.end(), entry_before);
}

bool name_matches(std::string_view name, std::string_view filter) {
    if (filter.empty())
        return true;
    return folded_ascii(name).find(folded_ascii(filter)) != std::string::npos;
}

void scroll(const Layout& layout, UiState& state, int32_t points) noexcept {
    const int64_t wanted = int64_t{state.scroll_points} + points;
    state.scroll_points =
        static_cast<int32_t>(std::clamp<int64_t>(wanted, 0, std::max(0, layout.scroll_max_points)));
}

} // namespace oa::ui::folder_chooser
