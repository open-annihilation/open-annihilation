// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Declared rows: the placement of a column of rows, as the settings dialog
// places a section's, their scroll, and what they draw and where their
// controls are. A row's kind is its view's, read from its spec.

#include "oa/ui/kit/rows.hpp"

#include "oa/ui/kit/text.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::ui::kit {

namespace {

constexpr const Metrics& kMetrics = compact_metrics;

/// Tells whether a rectangle has an area.
///
/// @param rect the rectangle
/// @return true when it is wider and taller than nothing
[[nodiscard]] bool has_area(const Rect& rect) noexcept {
    return rect.width > 0 && rect.height > 0;
}

/// Returns the width of the control a row keeps at its label line's right.
///
/// @param view the row
/// @return the control's width, in points; 0 for a row whose control, if it
///     has one, stands elsewhere
/// Returns one button of a buttons row. Measured widths, when the view has
/// them, replace the class's folder widths.
///
/// @param view the row
/// @param area the row's buttons
/// @param at the button, from the left
/// @return the button; empty when `at` is past the row's buttons
[[nodiscard]] Rect button_rect(const RowView& view, const Rect& area, std::size_t at) noexcept {
    if (view.button_widths.empty())
        return row_button(area, at);
    if (at >= view.button_widths.size())
        return {area.x, area.y, 0, 0};
    int32_t left = area.x;
    for (std::size_t before = 0; before < at; ++before)
        left += view.button_widths[before] + kMetrics.folder_button_gap;
    return {left, area.y, view.button_widths[at], area.height};
}

[[nodiscard]] int32_t label_line_control_width(const RowView& view) noexcept {
    switch (view.kind) {
    case RowKind::toggle:
        return std::max(kMetrics.switch_width, view.control_width);
    case RowKind::levels:
        return view.count * view.control_width + 2;
    case RowKind::value_and_button:
        return view.control_width > 0 ? view.control_width : kMetrics.button_row_width;
    case RowKind::buttons: {
        if (!view.button_widths.empty()) {
            int32_t width = 0;
            for (std::size_t at = 0; at < view.button_widths.size(); ++at)
                width += view.button_widths[at] + (at > 0 ? kMetrics.folder_button_gap : 0);
            return width;
        }
        int32_t width = 0;
        const std::size_t count =
            std::min(view.buttons.size(), kMetrics.folder_button_widths.size());
        for (std::size_t at = 0; at < count; ++at)
            width += kMetrics.folder_button_widths[at] + (at > 0 ? kMetrics.folder_button_gap : 0);
        return width;
    }
    case RowKind::choice:
    case RowKind::slider:
    case RowKind::text_field:
    case RowKind::link:
    case RowKind::text:
        break;
    }
    return 0;
}

/// Returns the control kind a row's control is listed as.
///
/// @param kind the row's kind
/// @return the control's kind; a value_and_button row's is a button
[[nodiscard]] ControlKind control_kind(RowKind kind) noexcept {
    switch (kind) {
    case RowKind::toggle:
        return ControlKind::toggle;
    case RowKind::choice:
        return ControlKind::choice;
    case RowKind::slider:
        return ControlKind::slider;
    case RowKind::levels:
        return ControlKind::levels;
    case RowKind::value_and_button:
        return ControlKind::button;
    case RowKind::buttons:
        return ControlKind::buttons;
    case RowKind::text_field:
        return ControlKind::text_field;
    case RowKind::link:
        return ControlKind::link;
    case RowKind::text:
        break;
    }
    return ControlKind::area;
}

/// Tells whether Left and Right act on a row's control.
///
/// @param kind the row's kind
/// @return true for every control but a button and a link
[[nodiscard]] bool takes_steps(RowKind kind) noexcept {
    switch (kind) {
    case RowKind::toggle:
    case RowKind::choice:
    case RowKind::slider:
    case RowKind::levels:
    case RowKind::buttons:
    case RowKind::text_field:
        return true;
    case RowKind::value_and_button:
    case RowKind::link:
    case RowKind::text:
        break;
    }
    return false;
}

/// Tells whether a row's hint lines are text from its host, kept to their
/// own columns, as the Game files rows' are.
///
/// @param kind the row's kind
/// @return true for a value_and_button and a text row
[[nodiscard]] bool host_text(RowKind kind) noexcept {
    switch (kind) {
    case RowKind::value_and_button:
    case RowKind::text:
        return true;
    case RowKind::toggle:
    case RowKind::choice:
    case RowKind::slider:
    case RowKind::levels:
    case RowKind::buttons:
    case RowKind::text_field:
    case RowKind::link:
        break;
    }
    return false;
}

/// Returns a row's control's name.
///
/// @param state the names' prefix
/// @param view the row
/// @return <prefix>.<id>, a value_and_button's with its button's word when that
///     word is not empty; the id alone without a prefix
[[nodiscard]] std::string control_name(const RowsState& state, const RowView& view) {
    std::string name = state.name_prefix;
    if (!name.empty())
        name += '.';
    name += view.id;
    // An empty button word is not part of the name. A value_and_button always
    // carries one button id, and an empty one would leave a trailing dot.
    if (view.kind == RowKind::value_and_button && !view.button_ids.empty() &&
        !view.button_ids.front().empty()) {
        name += '.';
        name += view.button_ids.front();
    }
    return name;
}

/// Returns what automation reads as a row's control's text.
///
/// @param view the row
/// @return a switch's caption, a strip's chosen level, a slider's, a
///     drop-down's or a value_and_button's value, a text field's text, a link's
///     caption; empty for a row of buttons
[[nodiscard]] std::string control_text(const RowView& view) {
    switch (view.kind) {
    case RowKind::toggle:
        return view.captions.size() == 2 ? view.captions[view.on ? 1 : 0] : std::string();
    case RowKind::levels:
        return view.index >= 0 && static_cast<std::size_t>(view.index) < view.captions.size()
                   ? view.captions[static_cast<std::size_t>(view.index)]
                   : std::string();
    case RowKind::choice:
    case RowKind::slider:
        return view.value;
    case RowKind::value_and_button:
    case RowKind::link:
        return view.buttons.empty() ? std::string() : view.buttons.front();
    case RowKind::text_field:
        return view.text;
    case RowKind::buttons:
    case RowKind::text:
        break;
    }
    return {};
}

/// Appends one item, in the rows' clip.
///
/// @param[in,out] list the display list
/// @param role what it draws
/// @param rect where
/// @param control the control it draws, or none
/// @param text its words
/// @param state how its control looks
/// @param look its component's look
/// @param clip the rows' clip
void push_item(
    DisplayList& list,
    Role role,
    const Rect& rect,
    ControlId control,
    std::string text,
    State state,
    Look look,
    const Rect& clip
) {
    Item item;
    item.role = role;
    item.rect = rect;
    item.clip = clip;
    item.text = std::move(text);
    item.control = control;
    item.state = state;
    item.look = std::move(look);
    list.items.push_back(std::move(item));
}

/// Appends a value's text, in the regular font.
///
/// @param[in,out] list the display list
/// @param rect its box
/// @param text the value, as shown
/// @param align where it sits in its box
/// @param ink its colour
/// @param clip the rows' clip
void push_value(
    DisplayList& list,
    const Rect& rect,
    const std::string& text,
    Align align,
    Colour ink,
    const Rect& clip
) {
    if (text.empty() || !has_area(rect))
        return;
    Item item;
    item.role = Role::text;
    item.rect = rect;
    item.clip = clip;
    item.text = text;
    item.font = FontRole::regular;
    item.align = align;
    item.colour = ink;
    list.items.push_back(std::move(item));
}

/// Appends a row's rule, label and hint lines.
///
/// @param[in,out] list the display list
/// @param placed the rows
/// @param row the row
/// @param clip the rows' clip
void push_frame(
    DisplayList& list, const PlacedRows& placed, const PlacedRow& row, const Rect& clip
) {
    RowFrame frame;
    frame.top = row.top;
    frame.left = placed.left;
    frame.width = placed.right - placed.left;
    frame.label = row.label;
    frame.label_text = row.view.label;
    const bool own_columns = host_text(row.view.kind);
    for (std::size_t line = 0; line < row.hints.size(); ++line) {
        const Rect& box = row.hints[line];
        frame.hints.push_back(box);
        frame.hint_texts.push_back(
            line < row.view.hints.size() ? row.view.hints[line] : std::string()
        );
        frame.hint_notices.push_back(line < row.view.notices.size() && row.view.notices[line]);
        if (!own_columns)
            frame.hint_clips.emplace_back();
        else if (has_area(clip))
            frame.hint_clips.push_back({box.x, clip.y, box.width, clip.height});
        else
            frame.hint_clips.push_back({box.x, row.top, box.width, row.height});
    }
    push_item(
        list,
        Role::row_frame,
        {placed.left, row.top, frame.width, row.height},
        no_control,
        row.view.label,
        {},
        std::move(frame),
        clip
    );
}

/// Appends a row's control as it looks, and a slider's or a row's value.
///
/// @param[in,out] list the display list
/// @param row the row
/// @param control its control, or none while it is locked
/// @param state the pointer and the focus
void push_control(
    DisplayList& list, const PlacedRow& row, ControlId control, const RowsState& state
) {
    const RowView& view = row.view;
    const Rect& area = row.control_area;
    const bool locked = !view.lock.empty();
    const bool live = control != no_control && view.enabled;
    const bool hovered = live && (state.hovered == control || state.pressed == control);
    const bool held = live && state.pressed == control && state.hovered == control;
    const Rect& clip = state.clip;
    State look_state;
    look_state.hovered = hovered;
    look_state.locked = locked;
    look_state.disabled = !view.enabled;
    switch (view.kind) {
    case RowKind::toggle: {
        if (!has_area(area))
            break;
        SwitchLook look;
        look.on = view.on;
        look.hovered = hovered;
        look.locked = locked;
        if (view.captions.size() == 2) {
            look.off_caption = view.captions[0];
            look.on_caption = view.captions[1];
        }
        look_state.on = view.on;
        push_item(list, Role::toggle, area, control, control_text(view), look_state, look, clip);
        break;
    }
    case RowKind::levels: {
        if (!has_area(area))
            break;
        LevelsLook look;
        look.captions = view.captions;
        look.level_width = view.control_width;
        look.chosen = static_cast<std::size_t>(std::max(view.index, int32_t{0}));
        look.offered = static_cast<std::size_t>(std::max(view.offered, int32_t{0}));
        look.hovered = hovered;
        look.locked = locked;
        push_item(list, Role::levels, area, control, control_text(view), look_state, look, clip);
        break;
    }
    case RowKind::slider: {
        if (has_area(area)) {
            const SliderLook look{view.index, view.count, locked, hovered};
            push_item(list, Role::slider, area, control, view.value, look_state, look, clip);
        }
        push_value(list, row.value, view.value, Align::right, colour::text, clip);
        break;
    }
    case RowKind::choice: {
        if (!has_area(area))
            break;
        const ChoiceLook look{
            view.value, hovered, control != no_control && state.open_menu == control
        };
        push_item(list, Role::choice, area, control, view.value, look_state, look, clip);
        break;
    }
    case RowKind::value_and_button: {
        push_value(
            list,
            row.value,
            view.value,
            view.label.empty() ? Align::left : Align::right,
            colour::hint,
            clip
        );
        if (!has_area(area) || view.buttons.empty())
            break;
        ButtonLook look;
        look.caption = view.buttons.front();
        // A row's button is a secondary action at the row's height; the
        // dialog's own OK is its one primary button. A risky one takes the
        // danger look, and loses it while locked, under the row's fade, or
        // while the row disables it.
        look.style = view.danger && view.enabled && view.lock.empty() ? ButtonStyle::danger
                                                                      : ButtonStyle::plain;
        look.hovered = hovered;
        look.held = held;
        look.enabled = view.enabled;
        look_state.pressed = held;
        push_item(list, Role::button, area, control, look.caption, look_state, look, clip);
        break;
    }
    case RowKind::buttons: {
        if (!has_area(area))
            break;
        for (std::size_t at = 0; at < view.buttons.size(); ++at) {
            const bool button_hovered =
                live && state.hovered == control && state.hovered_button == at;
            const bool button_held = button_hovered && state.pressed == control;
            ButtonLook look;
            look.caption = view.buttons[at];
            look.style = ButtonStyle::plain;
            look.hovered = button_hovered;
            look.held = button_held;
            look.enabled = view.enabled;
            State button_state = look_state;
            button_state.hovered = button_hovered;
            button_state.pressed = button_held;
            push_item(
                list,
                Role::button,
                button_rect(view, area, at),
                control,
                look.caption,
                button_state,
                look,
                clip
            );
        }
        break;
    }
    case RowKind::text_field: {
        if (!has_area(area))
            break;
        const bool focused = control != no_control && state.focused == control;
        SearchLook look;
        look.field.text = view.text;
        look.field.caret = focused ? std::min(state.caret, view.text.size()) : view.text.size();
        look.focused = focused;
        look.hovered = hovered;
        look.magnifier = false;
        look_state.focused = focused;
        push_item(list, Role::field, area, control, view.text, look_state, look, clip);
        break;
    }
    case RowKind::link: {
        if (!has_area(area) || view.buttons.empty())
            break;
        const LinkLook look{view.buttons.front(), hovered, view.enabled};
        push_item(list, Role::link, area, control, look.caption, look_state, look, clip);
        break;
    }
    case RowKind::text:
        push_value(
            list,
            row.value,
            view.value,
            view.label.empty() ? Align::left : Align::right,
            colour::hint,
            clip
        );
        break;
    }
}

} // namespace

std::string shown_copy(const ShownText& shown, std::string_view english) {
    return std::string(shown ? shown(english) : english);
}

void row_id_is_not_one_word() noexcept {
}

namespace {

/// The most bytes a preference key may hold.
constexpr std::size_t kPreferenceKeyMost = 64;

/// Tells whether a key's word is a to z, 0 to 9 and hyphens.
///
/// @param word the word
/// @return true when it is one such word
[[nodiscard]] bool key_word(std::string_view word) noexcept {
    return row_word(word);
}

/// Returns the text stored for a key, or null when the key is absent.
///
/// @param model the values
/// @param key the key
/// @return the stored text; null when none is stored
[[nodiscard]] const std::string* stored_text(const PreferenceModel& model, std::string_view key) {
    const auto found = model.values.find(std::string(key));
    if (found == model.values.end())
        return nullptr;
    return &found->second;
}

/// Writes a key's text.
///
/// @param[in,out] model the values
/// @param key the key
/// @param text the text
void store_text(PreferenceModel& model, std::string_view key, std::string text) {
    model.values[std::string(key)] = std::move(text);
}

/// Tells whether a text is the toggle's On.
///
/// @param text the stored text
/// @return true for 1
[[nodiscard]] bool toggle_on(std::string_view text) noexcept {
    return text == "1";
}

/// Returns how many stops a preference slider has.
///
/// @param spec the row
/// @return the stops; 1 when the step is not a step
[[nodiscard]] int32_t slider_stops(const PreferenceRow& spec) noexcept {
    if (!(spec.slider_step > 0.0) || spec.slider_most < spec.slider_least)
        return 1;
    const double span = (spec.slider_most - spec.slider_least) / spec.slider_step;
    const auto count = static_cast<int32_t>(std::llround(span)) + 1;
    return count < 1 ? 1 : count;
}

/// Returns a slider stop's value.
///
/// @param spec the row
/// @param index the stop, from 0
/// @return the value
[[nodiscard]] double slider_value(const PreferenceRow& spec, int32_t index) noexcept {
    return spec.slider_least + static_cast<double>(index) * spec.slider_step;
}

/// Writes a decimal with no trailing zeros.
///
/// @param value the value
/// @return its text
[[nodiscard]] std::string format_decimal(double value) {
    std::ostringstream stream;
    stream.setf(std::ios::fixed);
    stream.precision(6);
    stream << value;
    std::string text = stream.str();
    const std::size_t dot = text.find('.');
    if (dot == std::string::npos)
        return text;
    while (!text.empty() && text.back() == '0')
        text.pop_back();
    if (!text.empty() && text.back() == '.')
        text.pop_back();
    if (text.empty() || text == "-")
        return "0";
    return text;
}

/// Reads a decimal with no other characters.
///
/// @param text the text
/// @param[out] value the value
/// @return true when the whole text is a decimal
[[nodiscard]] bool parse_decimal(std::string_view text, double& value) noexcept {
    if (text.empty())
        return false;
    std::size_t index = 0;
    double sign = 1.0;
    if (text[index] == '+' || text[index] == '-') {
        if (text[index] == '-')
            sign = -1.0;
        ++index;
    }
    if (index >= text.size() || text[index] < '0' || text[index] > '9')
        return false;
    double whole = 0.0;
    while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
        whole = whole * 10.0 + static_cast<double>(text[index] - '0');
        ++index;
    }
    double fraction = 0.0;
    if (index < text.size() && text[index] == '.') {
        ++index;
        if (index >= text.size())
            return false;
        double place = 0.1;
        while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
            fraction += static_cast<double>(text[index] - '0') * place;
            place *= 0.1;
            ++index;
        }
    }
    if (index != text.size())
        return false;
    value = sign * (whole + fraction);
    return true;
}

/// Returns the stop a stored decimal names, or -1 when it is not on the step.
///
/// @param spec the row
/// @param text the stored text
/// @return the stop, from 0; -1 when the text is not a stop
[[nodiscard]] int32_t slider_stop_of(const PreferenceRow& spec, std::string_view text) noexcept {
    double value = 0.0;
    if (!parse_decimal(text, value) || !(spec.slider_step > 0.0))
        return -1;
    const double steps = (value - spec.slider_least) / spec.slider_step;
    const double nearest = std::round(steps);
    if (std::abs(steps - nearest) > 1e-4)
        return -1;
    const auto index = static_cast<int32_t>(nearest);
    if (index < 0 || index >= slider_stops(spec))
        return -1;
    return index;
}

/// Returns a choice's index for an id, or -1 when the id is not one of them.
///
/// @param spec the row
/// @param id the choice's id
/// @return the index, from 0; -1 when it is not a choice
[[nodiscard]] int32_t choice_index_of(const PreferenceRow& spec, std::string_view id) noexcept {
    for (std::size_t index = 0; index < spec.choice_ids.size(); ++index)
        if (spec.choice_ids[index] == id)
            return static_cast<int32_t>(index);
    return -1;
}

/// Returns a text cut to the bytes a preference field keeps.
///
/// @param text the text
/// @return the text, at most preference_text_most bytes, on a byte boundary
[[nodiscard]] std::string capped_text(std::string_view text) {
    if (text.size() <= preference_text_most)
        return std::string(text);
    return std::string(text.substr(0, preference_text_most));
}

} // namespace

const char* preference_key_problem(std::string_view key) noexcept {
    if (key.size() > kPreferenceKeyMost)
        return "a preference key is longer than 64 bytes";
    if (key.starts_with("open-annihilation."))
        return "a preference key uses the engine's own prefix";
    if (key.empty() || key.front() == '.' || key.back() == '.')
        return "a preference key is not dotted words";
    std::size_t words = 0;
    std::size_t start = 0;
    for (std::size_t index = 0; index <= key.size(); ++index) {
        if (index != key.size() && key[index] != '.')
            continue;
        if (index == start || !key_word(key.substr(start, index - start)))
            return "a preference key is not dotted words";
        ++words;
        start = index + 1;
    }
    if (words < 2)
        return "a preference key is not dotted words";
    return nullptr;
}

PreferenceRow preference_toggle(
    std::string_view id,
    std::string_view label,
    std::string_view key,
    std::string_view default_text,
    std::array<std::string_view, 2> hint
) {
    PreferenceRow spec(RowKind::toggle, id);
    spec.label = label;
    spec.key = key;
    spec.default_text = default_text;
    spec.hint = hint;
    return spec;
}

PreferenceRow preference_choice(
    std::string_view id,
    std::string_view label,
    std::string_view key,
    std::string_view default_text,
    std::span<const std::string_view> choice_ids,
    std::span<const std::string_view> captions,
    std::array<std::string_view, 2> hint
) {
    PreferenceRow spec(RowKind::choice, id);
    spec.label = label;
    spec.key = key;
    spec.default_text = default_text;
    spec.choice_ids = choice_ids;
    spec.captions = captions;
    spec.hint = hint;
    return spec;
}

PreferenceRow preference_slider(
    std::string_view id,
    std::string_view label,
    std::string_view key,
    std::string_view default_text,
    double least,
    double most,
    double step,
    std::array<std::string_view, 2> hint
) {
    PreferenceRow spec(RowKind::slider, id);
    spec.label = label;
    spec.key = key;
    spec.default_text = default_text;
    spec.slider_least = least;
    spec.slider_most = most;
    spec.slider_step = step;
    spec.hint = hint;
    return spec;
}

PreferenceRow preference_text_field(
    std::string_view id,
    std::string_view label,
    std::string_view key,
    std::string_view default_text,
    std::array<std::string_view, 2> hint
) {
    PreferenceRow spec(RowKind::text_field, id);
    spec.label = label;
    spec.key = key;
    spec.default_text = default_text;
    spec.hint = hint;
    return spec;
}

bool preference_stored_on(const PreferenceModel& model, const PreferenceRow& spec) {
    const std::string* stored = stored_text(model, spec.key);
    if (stored == nullptr || (*stored != "0" && *stored != "1"))
        return toggle_on(spec.default_text);
    return toggle_on(*stored);
}

void preference_store_on(PreferenceModel& model, const PreferenceRow& spec, bool on) {
    store_text(model, spec.key, on ? "1" : "0");
}

int32_t preference_stored_count(const PreferenceRow& spec) {
    if (spec.kind == RowKind::choice)
        return static_cast<int32_t>(spec.captions.size());
    if (spec.kind == RowKind::slider)
        return slider_stops(spec);
    return 0;
}

int32_t preference_stored_index(const PreferenceModel& model, const PreferenceRow& spec) {
    const std::string* stored = stored_text(model, spec.key);
    if (spec.kind == RowKind::choice) {
        const int32_t found = stored != nullptr ? choice_index_of(spec, *stored) : -1;
        if (found >= 0)
            return found;
        const int32_t fallback = choice_index_of(spec, spec.default_text);
        return fallback >= 0 ? fallback : 0;
    }
    if (spec.kind == RowKind::slider) {
        const int32_t found = stored != nullptr ? slider_stop_of(spec, *stored) : -1;
        if (found >= 0)
            return found;
        const int32_t fallback = slider_stop_of(spec, spec.default_text);
        return fallback >= 0 ? fallback : 0;
    }
    return 0;
}

void preference_store_index(PreferenceModel& model, const PreferenceRow& spec, int32_t chosen) {
    if (chosen < 0)
        return;
    if (spec.kind == RowKind::choice) {
        if (static_cast<std::size_t>(chosen) >= spec.choice_ids.size())
            return;
        store_text(model, spec.key, std::string(spec.choice_ids[static_cast<std::size_t>(chosen)]));
        return;
    }
    if (spec.kind == RowKind::slider) {
        if (chosen >= slider_stops(spec))
            return;
        store_text(model, spec.key, format_decimal(slider_value(spec, chosen)));
    }
}

std::string preference_stored_caption(const PreferenceRow& spec, int32_t at) {
    if (at < 0)
        return {};
    if (spec.kind == RowKind::choice) {
        if (static_cast<std::size_t>(at) >= spec.captions.size())
            return {};
        return std::string(spec.captions[static_cast<std::size_t>(at)]);
    }
    if (spec.kind == RowKind::slider) {
        if (at >= slider_stops(spec))
            return {};
        return format_decimal(slider_value(spec, at));
    }
    return {};
}

std::string preference_stored_choice_id(const PreferenceRow& spec, int32_t at) {
    if (spec.kind != RowKind::choice || at < 0 ||
        static_cast<std::size_t>(at) >= spec.choice_ids.size())
        return {};
    return std::string(spec.choice_ids[static_cast<std::size_t>(at)]);
}

std::string preference_stored_text(const PreferenceModel& model, const PreferenceRow& spec) {
    const std::string* stored = stored_text(model, spec.key);
    if (stored == nullptr || stored->size() > preference_text_most)
        return capped_text(spec.default_text);
    return *stored;
}

void preference_store_text(PreferenceModel& model, const PreferenceRow& spec, std::string text) {
    if (text.size() > preference_text_most)
        text.resize(preference_text_most);
    store_text(model, spec.key, std::move(text));
}

PlacedRows place_rows(std::span<const RowView> views, const RowPlacement& placement) {
    PlacedRows placed;
    placed.left = placement.left;
    placed.right = placement.right;
    const int32_t width = placement.right - placement.left;
    const int32_t line_height = kMetrics.regular_line;
    int32_t top = placement.first_top;
    placed.rows.reserve(views.size());
    for (std::size_t at = 0; at < views.size(); ++at) {
        PlacedRow& row = placed.rows.emplace_back();
        row.view = views[at];
        const RowView& view = row.view;
        row.control = placement.first_control + static_cast<int32_t>(at);
        row.top = top;
        const int32_t label_top = top + 1 + placement.row_padding;
        const bool locked = !view.lock.empty();
        int32_t lock_width = view.lock_columns > 0 ? view.lock_columns : kMetrics.lock_width;
        int32_t label_right = placement.right;
        const int32_t control_width = label_line_control_width(view);
        // A control that does not fit beside its label stands on the next
        // line. The lock stays on the label line — immediately left of a
        // control that sits there, otherwise at the line's right — unless
        // the label and the lock do not fit on that line. Then the lock
        // moves to the start of the line under the label, before the hint,
        // and the label keeps the whole line, or the room beside a control
        // that stays. A slider's track, a drop-down and a text field stay
        // on a line of their own either way. A lock never hides a control,
        // and a label is never narrowed below its text to make room for a lock.
        const bool drop_line = view.control_below && control_width > 0;
        const bool on_label = control_width > 0 && !drop_line && view.kind != RowKind::link;
        bool lock_below = locked && view.lock_below;
        // No measured lock: decide from the estimate. A control on the label
        // line counts, with the gap before it. A slider, a drop-down, a text
        // field or a text row has only the label and the lock on that line.
        if (locked && !lock_below && view.lock_columns <= 0 && !view.label.empty()) {
            const int32_t label_columns = estimated_width(view.label);
            const int32_t beside = label_columns + kMetrics.label_gap + lock_width;
            if (on_label) {
                if (beside + kMetrics.label_gap + control_width > width)
                    lock_below = true;
            } else if (beside > width) {
                lock_below = true;
            }
        }
        if (lock_below) {
            // A measured lock is exactly its icon and words. Without a
            // measure, the box is at least the class's, and wide enough for
            // the estimated words, so a long lock is not cut. Right-aligned
            // inside that box, the words start at the line's left.
            int32_t dropped = lock_width;
            if (view.lock_columns <= 0) {
                const int32_t words =
                    estimated_width(view.lock) + kMetrics.padlock_width + kMetrics.padlock_gap;
                dropped = std::max(kMetrics.lock_width, words);
            }
            lock_width = std::min(dropped, std::max(width, int32_t{1}));
        }
        // A switch and a level strip grow with the class. A button stays on
        // the regular line, as does a lock that has dropped under the label.
        const bool grows = view.kind == RowKind::toggle || view.kind == RowKind::levels;
        const int32_t grown = placement.control_height > 0 ? placement.control_height : line_height;
        const int32_t control_line = grows ? grown : line_height;
        const int32_t label_line = on_label && grows ? grown : line_height;
        int32_t control_top = label_top;
        if (drop_line) {
            control_top = label_top + line_height + kMetrics.hint_gap;
            if (lock_below)
                control_top += line_height + kMetrics.hint_gap;
        }
        if (drop_line) {
            row.control_area = {
                placement.right - control_width, control_top, control_width, control_line
            };
        } else if (on_label) {
            row.control_area = {
                placement.right - control_width, label_top, control_width, control_line
            };
        }
        if (locked && lock_below) {
            row.lock_area = {
                placement.left, label_top + line_height + kMetrics.hint_gap, lock_width, line_height
            };
            if (on_label)
                label_right = row.control_area.x - kMetrics.label_gap;
        } else if (locked) {
            const int32_t lock_height = on_label && grows ? grown : line_height;
            if (on_label) {
                row.lock_area = {
                    row.control_area.x - kMetrics.label_gap - lock_width,
                    label_top,
                    lock_width,
                    lock_height
                };
            } else {
                row.lock_area = {placement.right - lock_width, label_top, lock_width, line_height};
            }
            label_right = row.lock_area.x - kMetrics.label_gap;
        } else if (on_label) {
            label_right = row.control_area.x - kMetrics.label_gap;
        }
        if (view.kind == RowKind::link) {
            // A link stands alone where the label would, short of its lock.
            const int32_t room = label_right - placement.left;
            const int32_t link_columns =
                view.control_width > 0 ? std::min(view.control_width, room) : room;
            row.control_area = {placement.left, label_top, link_columns, line_height};
        } else {
            row.label = {placement.left, label_top, label_right - placement.left, label_line};
        }
        // Hints follow whichever of the label, a lock and a control on these
        // lines extends lowest. With every part regular_line high this is the
        // old followed + line_height.
        int32_t occupied = label_top;
        occupied += view.kind == RowKind::link ? line_height : row.label.height;
        if (has_area(row.lock_area))
            occupied = std::max(occupied, row.lock_area.y + row.lock_area.height);
        if (has_area(row.control_area) && view.kind != RowKind::link)
            occupied = std::max(occupied, row.control_area.y + row.control_area.height);
        int32_t bottom = occupied + kMetrics.hint_gap;
        for (std::size_t line = 0; line < view.hints.size(); ++line) {
            if (line > 0 && placement.tall)
                bottom += kMetrics.tall_hint_line_gap;
            row.hints.push_back({placement.left, bottom, width, kMetrics.small_line});
            bottom += kMetrics.small_line;
        }
        switch (view.kind) {
        case RowKind::slider: {
            bottom += kMetrics.slider_gap;
            const int32_t wanted =
                view.value_columns > 0 ? view.value_columns : kMetrics.slider_value_width;
            const int32_t room = std::max(width - kMetrics.slider_value_gap, int32_t{1});
            const int32_t value_width = std::min(std::max(wanted, int32_t{1}), room);
            const int32_t value_left = placement.right - value_width;
            row.control_area = {
                placement.left,
                bottom,
                std::max(value_left - kMetrics.slider_value_gap - placement.left, int32_t{0}),
                kMetrics.slider_line_height
            };
            row.value = {value_left, bottom, value_width, kMetrics.slider_line_height};
            bottom += kMetrics.slider_line_height;
            break;
        }
        case RowKind::choice:
        case RowKind::text_field: {
            // A drop-down's field and a text field stand on their own line,
            // as a slider's track does.
            bottom += kMetrics.slider_gap;
            const int32_t field_height =
                view.kind == RowKind::choice
                    ? grown
                    : (placement.field_height > 0 ? placement.field_height : kMetrics.field_height);
            row.control_area = {
                placement.left,
                bottom,
                view.control_width > 0 ? view.control_width : kMetrics.choice_width,
                field_height
            };
            bottom += field_height;
            break;
        }
        case RowKind::value_and_button:
            // A label and a value that do not fit on one line with the button:
            // the label keeps the first line and the value stands on the
            // control's line, ending label_gap before the button. The lock
            // stays on the label line unless it has dropped under the label.
            // An empty label leaves the value the first line.
            if (drop_line && !view.label.empty() && has_area(row.control_area)) {
                const int32_t value_right = row.control_area.x - kMetrics.label_gap;
                row.value = {
                    placement.left,
                    row.control_area.y,
                    std::max(value_right - placement.left, int32_t{0}),
                    line_height
                };
            } else {
                row.value = row.label;
            }
            break;
        case RowKind::text:
            // The value shares the label's box on the label line.
            row.value = row.label;
            break;
        case RowKind::toggle:
        case RowKind::levels:
        case RowKind::buttons:
        case RowKind::link:
            break;
        }
        bottom += placement.row_padding;
        row.height = bottom - top;
        top = bottom;
    }
    placed.bottom = top;
    return placed;
}

void scroll(PlacedRows& placed, int32_t by) noexcept {
    const auto lift = [by](Rect& rect) {
        if (has_area(rect))
            rect.y -= by;
    };
    for (PlacedRow& row : placed.rows) {
        row.top -= by;
        lift(row.label);
        lift(row.lock_area);
        for (Rect& hint : row.hints)
            lift(hint);
        lift(row.control_area);
        lift(row.value);
    }
    placed.bottom -= by;
}

ScrollArea rows_scroll(
    const PlacedRows& placed,
    const Rect& view,
    const Rect& well,
    const Rect& hit,
    int32_t page_step,
    int32_t end_gap,
    bool tall
) {
    ScrollArea area;
    area.view = view;
    area.well = well;
    area.hit = hit;
    area.page_step = page_step;
    area.content_height = placed.bottom + 1 + end_gap - view.y;
    area.limit = ScrollArea::limit_for(area.content_height, view.height);
    if (area.limit > 0 && tall) {
        // While the words are drawn in the modern fonts, whose ideographs
        // fill a hint line from its top row, the view's top edge cuts no hint
        // line at the end of the scroll: the rows scroll on until the line has
        // passed the edge.
        for (bool moved = true; moved;) {
            moved = false;
            for (const PlacedRow& row : placed.rows)
                for (const Rect& box : row.hints) {
                    const int32_t box_top = box.y - area.limit;
                    if (box_top <= view.y && box_top + box.height > view.y) {
                        area.limit += box_top + box.height - view.y;
                        moved = true;
                    }
                }
        }
        area.content_height = area.limit + view.height;
    }
    return area;
}

Rect row_button(const Rect& area, std::size_t at) noexcept {
    const std::size_t count = kMetrics.folder_button_widths.size();
    if (at >= count)
        return {area.x, area.y, 0, 0};
    int32_t left = area.x;
    for (std::size_t before = 0; before < at; ++before)
        left += kMetrics.folder_button_widths[before] + kMetrics.folder_button_gap;
    return {left, area.y, kMetrics.folder_button_widths[at], area.height};
}

std::size_t row_button_at(const Rect& area, std::size_t count, Point point) noexcept {
    const std::size_t buttons = std::min(count, kMetrics.folder_button_widths.size());
    for (std::size_t at = 0; at < buttons; ++at)
        if (contains(row_button(area, at), point))
            return at;
    return count;
}

void add_rows(DisplayList& list, const PlacedRows& placed, const RowsState& state) {
    const Rect& clip = state.clip;
    for (const PlacedRow& row : placed.rows) {
        const RowView& view = row.view;
        const bool locked = !view.lock.empty();
        const bool has_control = has_area(row.control_area) && !locked;
        const ControlId control = has_control ? row.control : no_control;
        push_frame(list, placed, row, clip);
        push_control(list, row, control, state);
        if (locked) {
            // A status is the player's explanation and keeps its strength.
            // Such a row fades its label line, and a lock or a control that
            // dropped under it, and stops at the status. Every other locked
            // row fades whole.
            int32_t cover_end = row.top + row.height;
            if (view.hint_is_status) {
                cover_end = row.lock_area.y + kMetrics.regular_line + kMetrics.hint_gap;
                if (has_area(row.control_area)) {
                    const int32_t control_end =
                        row.control_area.y + row.control_area.height + kMetrics.hint_gap;
                    if (control_end > cover_end)
                        cover_end = control_end;
                }
            }
            const int32_t faded = cover_end - (row.top + 1);
            push_item(
                list,
                Role::locked_fade,
                {placed.left, row.top + 1, placed.right - placed.left, faded},
                no_control,
                {},
                {},
                {},
                clip
            );
            push_item(
                list,
                Role::lock,
                row.lock_area,
                no_control,
                view.lock,
                {},
                LockLook{view.lock},
                clip
            );
        }
        if (has_control && state.focused == control) {
            // A row of buttons rings the button the keys mark.
            const Rect ring = view.kind == RowKind::buttons
                                  ? button_rect(view, row.control_area, state.marked_button)
                                  : row.control_area;
            push_item(list, Role::focus_ring, ring, no_control, {}, {}, FocusRingLook{true}, clip);
        }
        if (!has_control)
            continue;
        Control entry;
        entry.id = control;
        entry.rect = row.control_area;
        entry.clip = clip;
        entry.enabled = view.enabled;
        entry.name = control_name(state, view);
        entry.kind = control_kind(view.kind);
        entry.steps = takes_steps(view.kind);
        entry.group = state.group;
        entry.checked = view.kind == RowKind::toggle && view.on;
        entry.text = control_text(view);
        if (view.kind == RowKind::buttons)
            entry.parts = view.button_ids;
        else if (view.kind == RowKind::levels || view.kind == RowKind::choice)
            entry.parts = view.choice_ids;
        list.controls.push_back(std::move(entry));
        if (view.enabled)
            list.tab_order.push_back(control);
    }
    Item rule;
    rule.role = Role::rule;
    rule.rect = {placed.left, placed.bottom, placed.right - placed.left, 1};
    rule.clip = clip;
    rule.colour = colour::rule;
    list.items.push_back(std::move(rule));
}

} // namespace oa::ui::kit
