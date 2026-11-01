// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Gadget model of the loaded multiplayer panels.
#include "oa/ui/frontend_multiplayer/panel.hpp"

#include "oa/ui/gui_input/gadget_panel.hpp"
#include "oa/ui/gui_layout/gui_gadget.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <variant>

namespace oa::ui::frontend_multiplayer {

namespace {

char fold(char c) noexcept {
    return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
}

bool names_equal(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (fold(a[i]) != fold(b[i]))
            return false;
    return true;
}

template <std::size_t N>
void copy_bounded(std::array<char, N>& out, std::string_view text) noexcept {
    const auto length = std::min(text.size(), N - 1);
    std::memcpy(out.data(), text.data(), length);
    out[length] = '\0';
}

template <std::size_t N>
std::string_view view(const std::array<char, N>& text) noexcept {
    return {text.data(), ::strnlen(text.data(), N)};
}

} // namespace

std::string_view control_name(const Control& control) noexcept {
    return view(control.name);
}

std::string_view control_text(const Control& control) noexcept {
    return view(control.text);
}

void set_control_name(Control& control, std::string_view name) noexcept {
    copy_bounded(control.name, name);
}

void set_control_text(Control& control, std::string_view text) noexcept {
    copy_bounded(control.text, text);
}

void set_control_link(Control& control, std::string_view link) noexcept {
    copy_bounded(control.link, link);
}

std::string_view control_link(const Control& control) noexcept {
    return view(control.link);
}

void panel_load(Panel& panel, std::string_view name, const ui::gui_layout::Layout& layout) {
    panel = Panel{};
    copy_bounded(panel.name, name);
    const auto count = std::min(layout.gadgets.size(), kPanelControls);
    for (std::size_t index = 0; index < count; ++index) {
        const auto& gadget = layout.gadgets[index];
        auto& control = panel.controls[index];
        set_control_name(control, gadget.common.name);
        control.type = static_cast<ControlType>(gadget.common.type);
        control.x = gadget.common.x;
        control.y = gadget.common.y;
        control.width = gadget.common.width;
        control.height = gadget.common.height;
        control.attributes = static_cast<uint32_t>(gadget.common.attributes);
        control.active = static_cast<uint8_t>(gadget.common.active != 0 ? 1 : 0);
        control.group = gadget.common.association;
        control.source = static_cast<int16_t>(index);
        if (const auto* button = std::get_if<ui::gui_layout::ButtonFields>(&gadget.fields)) {
            set_control_text(control, button->text);
            control.stage = static_cast<uint8_t>(button->status);
            control.stages = static_cast<uint8_t>(button->stages);
            control.grayed = button->grayed_out;
            control.quick_key = button->quick_key;
        } else if (const auto* box = std::get_if<ui::gui_layout::TextBoxFields>(&gadget.fields)) {
            set_control_text(control, box->text);
        } else if (const auto* label = std::get_if<ui::gui_layout::LabelFields>(&gadget.fields)) {
            set_control_text(control, label->text);
        } else if (const auto* list = std::get_if<ui::gui_layout::ListBoxFields>(&gadget.fields)) {
            control.list_item_height = list->item_height;
        } else if (gadget.common.type == ui::gui_layout::GadgetType::scroll_bar) {
            control.scroll = ui::gui_input::scroll_bar_from_gadget(gadget);
        } else if (
            const auto* surface = std::get_if<ui::gui_layout::HotSurfaceFields>(&gadget.fields)
        ) {
            control.hot = surface->hot;
        }
    }
    panel.count = static_cast<int32_t>(count);
    if (count > 0) {
        panel.root_x = panel.controls[0].x;
        panel.root_y = panel.controls[0].y;
        if (const auto* root = std::get_if<ui::gui_layout::PanelFields>(&layout.gadgets[0].fields);
            root != nullptr && !root->default_focus.empty())
            panel.focus = panel_find(panel, root->default_focus);
    }
    panel.dirty = true;
}

int32_t panel_find(const Panel& panel, std::string_view name) noexcept {
    for (int32_t index = 1; index < panel.count; ++index)
        if (names_equal(control_name(panel.controls[static_cast<std::size_t>(index)]), name))
            return index;
    for (int32_t index = 1; index < panel.count; ++index)
        if (names_equal(control_link(panel.controls[static_cast<std::size_t>(index)]), name))
            return index;
    return kNoControl;
}

Control* panel_control(Panel& panel, std::string_view name) noexcept {
    const auto index = panel_find(panel, name);
    return index == kNoControl ? nullptr : &panel.controls[static_cast<std::size_t>(index)];
}

const Control* panel_control(const Panel& panel, std::string_view name) noexcept {
    const auto index = panel_find(panel, name);
    return index == kNoControl ? nullptr : &panel.controls[static_cast<std::size_t>(index)];
}

bool panel_selected_is(const Panel& panel, std::string_view name) noexcept {
    if (panel.selected <= 0 || panel.selected >= panel.count)
        return false;
    const auto& control = panel.controls[static_cast<std::size_t>(panel.selected)];
    return names_equal(control_name(control), name) || names_equal(control_link(control), name);
}

void panel_set_stage(Panel& panel, std::string_view name, int32_t stage) noexcept {
    if (auto* control = panel_control(panel, name)) {
        control->stage = static_cast<uint8_t>(stage);
        panel.dirty = true;
    }
}

void panel_set_value(Panel& panel, std::string_view name, int32_t value) noexcept {
    if (auto* control = panel_control(panel, name)) {
        control->value = static_cast<int16_t>(value);
        panel.dirty = true;
    }
}

void panel_set_active(Panel& panel, std::string_view name, bool active) noexcept {
    if (auto* control = panel_control(panel, name)) {
        control->active = active ? 1 : 0;
        panel.dirty = true;
    }
}

void panel_set_grayed(Panel& panel, std::string_view name, bool grayed) noexcept {
    if (auto* control = panel_control(panel, name)) {
        control->grayed = grayed;
        panel.dirty = true;
    }
}

void panel_set_text(Panel& panel, std::string_view name, std::string_view text) noexcept {
    auto* control = panel_control(panel, name);
    if (control == nullptr)
        return;
    set_control_text(*control, text);
    panel.dirty = true;
    if (control->type != ControlType::button)
        return;
    const auto caption = control_text(*control);
    const auto rule =
        oa::ui::gui_input::caption_quick_key(control->attributes, control->stages, caption);
    if (rule == oa::ui::gui_input::CaptionQuickKey::keep)
        return;
    control->quick_key = 0;
    if (rule == oa::ui::gui_input::CaptionQuickKey::none)
        return;
    std::array<int8_t, kPanelControls> taken{};
    const auto count = static_cast<std::size_t>(
        std::clamp<int32_t>(panel.count, 0, static_cast<int32_t>(kPanelControls))
    );
    for (std::size_t index = 0; index < count; ++index)
        if (panel.controls[index].type == ControlType::button)
            taken[index] = panel.controls[index].quick_key;
    control->quick_key =
        static_cast<int8_t>(oa::ui::gui_input::free_quick_key(caption, {taken.data(), count}));
}

std::string_view panel_text(const Panel& panel, std::string_view name) noexcept {
    const auto* control = panel_control(panel, name);
    return control == nullptr ? std::string_view{} : control_text(*control);
}

void panel_set_items(Panel& panel, std::string_view name, std::vector<std::string> items) {
    if (auto* control = panel_control(panel, name)) {
        control->items = std::move(items);
        control->item_keys.clear();
        if (control->list_selection >= static_cast<int16_t>(control->items.size()))
            control->list_selection = 0;
        panel.dirty = true;
    }
}

namespace gui_input = oa::ui::gui_input;

namespace {

/// Returns a control of the panel by index.
Control& at(Panel& panel, int32_t index) noexcept {
    return panel.controls[static_cast<std::size_t>(index)];
}

/// Returns a list's rows as the engine's list keeps them.
gui_input::ScrollList list_of(const Control& list) noexcept {
    gui_input::ScrollList rows;
    rows.y = list.y;
    rows.height = list.height;
    rows.item_height = list.list_item_height;
    rows.count = static_cast<int16_t>(
        std::min<std::size_t>(list.items.size(), std::numeric_limits<int16_t>::max())
    );
    rows.first = list.list_first;
    rows.selection = list.list_selection;
    rows.last_first = list.list_last_first;
    rows.group = list.group;
    rows.active = list.active != 0;
    return rows;
}

/// Stores the engine's list in its control.
void store_list(Control& list, const gui_input::ScrollList& rows) noexcept {
    list.height = rows.height;
    list.list_item_height = rows.item_height;
    list.list_first = rows.first;
    list.list_selection = rows.selection;
    list.list_last_first = rows.last_first;
}

/// Returns the first slider of a control's group, or null.
Control* group_bar(Panel& panel, int32_t index) noexcept {
    const auto bar = panel_group_member(panel, index, ControlType::slider);
    return bar == kNoControl ? nullptr : &at(panel, bar);
}

/// Tells whether a list shares a slider's group: the slider scrolls it.
bool scrolls_list(const Panel& panel, int32_t slider) noexcept {
    return panel_group_member(panel, slider, ControlType::list_box) != kNoControl;
}

} // namespace

int32_t panel_group_member(const Panel& panel, int32_t index, ControlType type) noexcept {
    if (index <= 0 || index >= panel.count)
        return kNoControl;
    const auto group = panel.controls[static_cast<std::size_t>(index)].group;
    for (int32_t other = 1; other < panel.count; ++other) {
        const auto& control = panel.controls[static_cast<std::size_t>(other)];
        if (other != index && control.type == type && control.group == group)
            return other;
    }
    return kNoControl;
}

void panel_bind_lists(Panel& panel) noexcept {
    for (int32_t index = 1; index < panel.count; ++index) {
        auto& list = at(panel, index);
        if (list.type != ControlType::list_box)
            continue;
        auto rows = list_of(list);
        gui_input::scroll_list_trim(rows, panel.line_height);
        store_list(list, rows);
    }
    panel.dirty = true;
}

void panel_fill_list(Panel& panel, std::string_view name, std::vector<std::string> rows) noexcept {
    const auto index = panel_find(panel, name);
    if (index == kNoControl)
        return;
    auto& list = at(panel, index);
    list.items = std::move(rows);
    list.item_keys.clear();
    list.item_grey.clear();
    auto filled = list_of(list);
    const bool overflow =
        gui_input::scroll_list_fill(filled, static_cast<int32_t>(filled.count), panel.line_height);
    store_list(list, filled);
    panel.dirty = true;
    if (list.active == 0)
        return;
    const auto bar = panel_group_member(panel, index, ControlType::slider);
    if (bar == kNoControl)
        return;
    at(panel, bar).active = overflow ? 1 : 0;
    // The knob is sized from the group's first list, as in 3.1c.
    if (overflow)
        gui_input::scroll_bar_fit_list(
            slider_bar(at(panel, bar)),
            list_of(at(panel, panel_group_member(panel, bar, ControlType::list_box))),
            panel.line_height
        );
}

void panel_set_item_keys(Panel& panel, std::string_view name, std::vector<std::string> keys) {
    auto* control = panel_control(panel, name);
    if (control == nullptr)
        return;
    if (keys.size() == control->items.size())
        control->item_keys = std::move(keys);
    else
        control->item_keys.clear();
    panel.dirty = true;
}

void panel_set_item_grey(Panel& panel, std::string_view name, std::vector<uint8_t> grey) {
    auto* control = panel_control(panel, name);
    if (control == nullptr)
        return;
    if (grey.size() == control->items.size())
        control->item_grey = std::move(grey);
    else
        control->item_grey.clear();
    panel.dirty = true;
}

void panel_select_list_row(Panel& panel, std::string_view name, int32_t row) noexcept {
    const auto index = panel_find(panel, name);
    if (index == kNoControl)
        return;
    auto& list = at(panel, index);
    auto rows = list_of(list);
    auto* bar = group_bar(panel, index);
    gui_input::scroll_list_select(
        rows, bar != nullptr ? &slider_bar(*bar) : nullptr, panel.line_height, row
    );
    store_list(list, rows);
    panel.dirty = true;
}

void panel_sync_group(Panel& panel, int32_t index) noexcept {
    if (index <= 0 || index >= panel.count)
        return;
    auto& source = at(panel, index);
    for (int32_t other = 1; other < panel.count; ++other) {
        auto& member = at(panel, other);
        if (other == index || member.group != source.group)
            continue;
        if (member.type == ControlType::list_box && source.type == ControlType::list_box) {
            member.list_first = source.list_first;
            member.list_selection = source.list_selection;
        } else if (member.type == ControlType::list_box && source.type == ControlType::slider) {
            auto rows = list_of(member);
            if (gui_input::scroll_list_follow_bar(rows, source.scroll))
                store_list(member, rows);
        } else if (member.type == ControlType::slider && source.type == ControlType::list_box) {
            gui_input::scroll_bar_follow_list(member.scroll, list_of(source));
        }
    }
    panel.dirty = true;
}

void panel_step_list(Panel& panel, int32_t index, bool forward) noexcept {
    if (index <= 0 || index >= panel.count)
        return;
    auto& list = at(panel, index);
    auto rows = list_of(list);
    auto* bar = group_bar(panel, index);
    const bool moved = gui_input::scroll_list_step(
        rows, bar != nullptr ? &slider_bar(*bar) : nullptr, panel.line_height, forward
    );
    store_list(list, rows);
    panel.dirty = true;
    if (moved)
        panel_sync_group(panel, index);
}

ListPress panel_press_list(Panel& panel, int32_t index, int32_t y) noexcept {
    if (index <= 0 || index >= panel.count)
        return ListPress::missed;
    auto& list = at(panel, index);
    int32_t row = 0;
    const auto press = gui_input::scroll_list_press(list_of(list), panel.line_height, y, row);
    if (press == ListPress::missed)
        return press;
    panel.focus = index;
    for (int32_t other = 1; other < panel.count; ++other) {
        auto& member = at(panel, other);
        if (member.type == ControlType::list_box && member.group == list.group)
            member.list_selection = static_cast<int16_t>(
                std::min<int32_t>(row, static_cast<int32_t>(member.items.size()) - 1)
            );
    }
    panel.dirty = true;
    return press;
}

int32_t list_shown_rows(const Control& list, int32_t line_height) noexcept {
    const int32_t pitch = list.list_item_height > 0 ? list.list_item_height : line_height;
    if (pitch <= 0 || list.height <= 0)
        return 1;
    const int32_t rows = static_cast<int32_t>(list.height) / pitch;
    return rows > 0 ? rows : 1;
}

bool panel_scroll_list(Panel& panel, int32_t index, int32_t rows) noexcept {
    if (index <= 0 || index >= panel.count)
        return false;
    auto& list = at(panel, index);
    if (list.type != ControlType::list_box)
        return false;
    auto scrolled = list_of(list);
    if (!gui_input::scroll_list_scroll(scrolled, rows))
        return false;
    store_list(list, scrolled);
    panel_sync_group(panel, index);
    return true;
}

int32_t panel_clone(Panel& panel, int32_t index, char suffix, int16_t dy) noexcept {
    if (index <= 0 || index >= panel.count || panel.count >= static_cast<int32_t>(kPanelControls))
        return kNoControl;
    const auto target = panel.count++;
    auto& copy = panel.controls[static_cast<std::size_t>(target)];
    copy = panel.controls[static_cast<std::size_t>(index)];
    const auto length = control_name(copy).size();
    if (length > 0)
        copy.name[length - 1] = suffix;
    copy.y = static_cast<int16_t>(copy.y + dy);
    return target;
}

bool panel_press(Panel& panel, int32_t index, uint8_t button) noexcept {
    if (index <= 0 || index >= panel.count)
        return false;
    auto& control = panel.controls[static_cast<std::size_t>(index)];
    if (control.grayed || control.active == 0 || control.display_only ||
        (control.type == ControlType::hot_surface && !control.hot))
        return false;
    if (control.type == ControlType::button) {
        if (control.stages > 1)
            control.stage = static_cast<uint8_t>((control.stage + 1) % control.stages);
        if ((control.attributes & kAttributeCheckbox) != 0)
            control.value = control.value != 0 ? 0 : 1;
    }
    panel.selected = index;
    panel.button = button;
    panel.dirty = true;
    return true;
}

bool panel_press(Panel& panel, std::string_view name, uint8_t button) noexcept {
    return panel_press(panel, panel_find(panel, name), button);
}

int32_t panel_hit(const Panel& panel, int32_t x, int32_t y) noexcept {
    for (int32_t index = panel.count - 1; index >= 1; --index) {
        const auto& control = panel.controls[static_cast<std::size_t>(index)];
        if (control.active == 0 || control.type == ControlType::label ||
            control.type == ControlType::panel || control.display_only ||
            (control.type == ControlType::hot_surface && !control.hot))
            continue;
        if (control.type == ControlType::slider &&
            control.scroll.art != gui_input::ScrollArt::unbound) {
            auto bar = control.scroll;
            bar.active = true;
            if (gui_input::scroll_bar_part(bar, x, y) != gui_input::ScrollPart::none)
                return index;
            continue;
        }
        if (x >= control.x && y >= control.y && x < control.x + control.width &&
            y < control.y + control.height)
            return index;
    }
    return kNoControl;
}

gui_input::ScrollBar& slider_bar(Control& slider) noexcept {
    slider.scroll.active = slider.active != 0;
    slider.scroll.grayed = slider.grayed;
    return slider.scroll;
}

void panel_bind_sliders(Panel& panel, const gui_input::ScrollArtFrames& art) noexcept {
    for (int32_t index = 1; index < panel.count; ++index) {
        auto& slider = panel.controls[static_cast<std::size_t>(index)];
        if (slider.type != ControlType::slider)
            continue;
        gui_input::bind_scroll_bar(slider.scroll, art);
        slider.x = slider.scroll.rect.x;
        slider.y = slider.scroll.rect.y;
        slider.width = slider.scroll.rect.width;
        slider.height = slider.scroll.rect.height;
        if (scrolls_list(panel, index))
            slider.active = 0;
    }
    panel.dirty = true;
}

int32_t slider_press(
    Panel& panel, gui_input::ScrollHold& hold, int32_t index, int32_t x, int32_t y, uint32_t tick
) noexcept {
    if (index <= 0 || index >= panel.count ||
        panel.controls[static_cast<std::size_t>(index)].type != ControlType::slider) {
        gui_input::scroll_let_go(hold);
        gui_input::scroll_move(hold, x, y);
        return kNoControl;
    }
    auto& bar = slider_bar(at(panel, index));
    if (!gui_input::scroll_press(bar, hold, index, x, y, tick))
        return kNoControl;
    panel.dirty = true;
    return index;
}

int32_t slider_release(Panel& panel, gui_input::ScrollHold& hold, int32_t x, int32_t y) noexcept {
    const auto held = hold.bar;
    if (held <= 0 || held >= panel.count) {
        gui_input::scroll_let_go(hold);
        return kNoControl;
    }
    if (!gui_input::scroll_release(slider_bar(at(panel, held)), hold, x, y))
        return kNoControl;
    panel.dirty = true;
    return held;
}

int32_t slider_hold_tick(Panel& panel, gui_input::ScrollHold& hold, uint32_t tick) noexcept {
    const auto held = hold.bar;
    if (held <= 0 || held >= panel.count) {
        gui_input::scroll_let_go(hold);
        return kNoControl;
    }
    if (!gui_input::scroll_hold_tick(slider_bar(at(panel, held)), hold, tick))
        return kNoControl;
    panel.dirty = true;
    return held;
}

} // namespace oa::ui::frontend_multiplayer
