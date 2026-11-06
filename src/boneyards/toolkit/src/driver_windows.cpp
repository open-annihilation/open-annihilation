// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The controls a program driving the game can list: each shown window's
// buttons, fields, lists, checks and labels, under the window's name.
#include <cstring>

#include "oa/boneyards/toolkit.hpp"

namespace oa::boneyards {
namespace {

// How many rows of one list are copied into one visit.
constexpr int32_t k_max_rows = 256;

/// Returns the window's own name when it has one.
///
/// @param window window to read; may be null
/// @return the name, or null when it has none
const char* own_name(const Window* window) {
    if (window == nullptr || window->name == nullptr || window->name[0] == '\0')
        return nullptr;
    return window->name;
}

/// Returns the caption the window shows.
///
/// @param window window to read
/// @return the caption, or "" when it shows none
const char* caption_of(Window* window) {
    if (window->ops == nullptr || window->ops->state_text == nullptr)
        return "";
    const char* text = window->ops->state_text(window, -1);
    return text != nullptr ? text : "";
}

/// Returns the name a program driving the game knows the control by.
///
/// The window's own name wins. A check created as BYCheckBox keeps that
/// window name and is reported by its caption, so each check in one console
/// is distinct. Without a name, the caption it shows is the name.
///
/// @param window control to name
/// @return the name, or null when the control has neither
const char* control_name(Window* window) {
    if (const char* name = own_name(window); name != nullptr) {
        if (std::strcmp(name, "BYCheckBox") == 0) {
            const char* caption = caption_of(window);
            if (caption[0] != '\0')
                return caption;
        }
        return name;
    }
    const char* caption = caption_of(window);
    return caption[0] != '\0' ? caption : nullptr;
}

/// Finds a descendant whose window name is `wanted`.
///
/// The pointer is that window's own name, and it stays valid for the visit.
///
/// @param window window to search
/// @param wanted the name
/// @return the name, or null when no descendant has it
const char* named_part(Window* window, const char* wanted) {
    if (window == nullptr || wanted == nullptr)
        return nullptr;
    for (int32_t index = 0; index < window->children.count; ++index) {
        Window* child = window->children.items[index];
        if (child == nullptr)
            continue;
        if (const char* name = own_name(child); name != nullptr && std::strcmp(name, wanted) == 0)
            return name;
        if (const char* nested = named_part(child, wanted); nested != nullptr)
            return nested;
    }
    return nullptr;
}

/// Tells whether the control takes a click.
///
/// A disabled button, check, radio or field does not.
///
/// @param window control to read
/// @return false when its state is the disabled one
bool control_enabled(const Window* window) {
    return window->states.current != button_state::disabled;
}

/// Tells whether a check or a radio shows its selected face.
///
/// @param window control to read
/// @return true when it is checked or selected
bool control_checked(const Window* window) {
    if (window->ops == nullptr || window->ops->is_class == nullptr)
        return false;
    if (window->ops->is_class(class_name::check_box)) {
        const int32_t state = window->states.current;
        return state == check_state::checked_down || state == check_state::checked_up;
    }
    if (window->ops->is_class(class_name::radio_button))
        return window->states.current == button_state::pressed;
    return false;
}

/// Returns the text a field holds, or the caption any other control shows.
///
/// @param window control to read
/// @return the text; never null
const char* control_text(Window* window) {
    if (window->ops != nullptr && window->ops->is_class != nullptr) {
        if (window->ops->is_class(class_name::single_edit))
            return single_edit_text(reinterpret_cast<const SingleEdit*>(window));
        if (window->ops->is_class(class_name::edit_box)) {
            const char* text = reinterpret_cast<const EditBox*>(window)->text;
            return text != nullptr ? text : "";
        }
    }
    return caption_of(window);
}

/// A masked field: its text is not reported, only how many characters it holds.
struct MaskedField {
    bool masked = false;
    int32_t length = 0;
};

/// Reads whether a field is masked and how many characters it holds.
///
/// @param window control to read
/// @return masked and the character count; unmasked for every other control
MaskedField masked_field(const Window* window) {
    MaskedField field;
    if (window->ops == nullptr || window->ops->is_class == nullptr)
        return field;
    if (window->ops->is_class(class_name::single_edit)) {
        const auto* edit = reinterpret_cast<const SingleEdit*>(window);
        if (edit->mask != '\0') {
            field.masked = true;
            field.length = edit->length > 0 ? edit->length : 0;
        }
    } else if (window->ops->is_class(class_name::edit_box)) {
        const auto* box = reinterpret_cast<const EditBox*>(window);
        if (box->mask != '\0') {
            field.masked = true;
            field.length = box->length > 0 ? box->length : 0;
        }
    }
    return field;
}

/// Classifies a window that a program driving the game can point at.
///
/// A plain panel, a frame and the windows that only hold other controls are
/// not controls of their own.
///
/// @param window window to classify
/// @param[out] kind receives the kind when this returns true
/// @return true when the window is such a control
bool classify(const Window* window, DriverControlKind* kind) {
    if (window->ops == nullptr || window->ops->is_class == nullptr)
        return false;
    const auto is = window->ops->is_class;
    if (is(class_name::list_box))
        *kind = DriverControlKind::list;
    else if (is(class_name::single_edit) || is(class_name::edit_box))
        *kind = DriverControlKind::text_field;
    else if (is(class_name::slider))
        *kind = DriverControlKind::slider;
    else if (is(class_name::check_box) || is(class_name::radio_button))
        *kind = DriverControlKind::check_box;
    else if (
        is(class_name::button) || is(class_name::no_focus_button) ||
        is(class_name::scroll_button) || is(class_name::stage_button)
    )
        *kind = DriverControlKind::button;
    else if (is(class_name::static_text))
        *kind = DriverControlKind::label;
    else if (is(class_name::progress))
        *kind = DriverControlKind::image;
    else
        return false;
    return true;
}

/// Returns the words a list row shows.
///
/// A row that draws its words with a label of its own (a player, a chat
/// frequency, a profile category) has no caption itself, so the first of
/// its children that shows any words stands for it: the name, ahead of an
/// icon or a count.
///
/// @param row the list row
/// @return the words, or "" when the row shows none
const char* row_caption(Window* row) {
    const char* caption = caption_of(row);
    if (caption[0] != '\0')
        return caption;
    for (int32_t index = 0; index < row->children.count; ++index) {
        Window* child = row->children.items[index];
        if (child == nullptr)
            continue;
        const char* shown = caption_of(child);
        if (shown[0] != '\0')
            return shown;
    }
    return "";
}

/// Fills a list control's rows from the list's items.
///
/// @param list the list
/// @param[out] items receives each row's caption
/// @param[out] view the control, whose list fields this fills
void fill_list(ListBox* list, const char* (&items)[k_max_rows], DriverControlView* view) {
    view->items = items;
    view->item_count = 0;
    view->first_visible = list->top > 0 ? list->top : 0;
    view->rows = list->page > 0 ? list->page : 0;
    view->row_height = 0;
    view->selected = -1;
    if (list->log_mode == 0 && list->count > 0 && list->selected >= 0 &&
        list->selected < list->count)
        view->selected = list->selected;
    for (int32_t index = 0; index < list->count && view->item_count < k_max_rows; ++index) {
        Window* item = list_box_item(list, index);
        if (item == nullptr)
            continue;
        if (view->row_height == 0)
            view->row_height = item->height;
        items[view->item_count] = row_caption(item);
        ++view->item_count;
    }
    if (view->selected >= 0 && view->selected < view->item_count)
        view->text = items[view->selected];
}

/// Reports one control. A list's rows stay part of the list. A list or a
/// slider also names its decbutton and incbutton.
///
/// @param window the control
/// @param kind what it is
/// @param owner the window it sits on
/// @param context passed to `visit`
/// @param visit the visit
void report(
    Window* window,
    DriverControlKind kind,
    const char* owner,
    void* context,
    DriverControlVisitor visit
) {
    const char* name = control_name(window);
    if (name == nullptr || owner == nullptr || owner[0] == '\0')
        return;
    const char* items[k_max_rows]{};
    DriverControlView view{};
    view.name = name;
    view.kind = kind;
    view.x = window->x;
    view.y = window->y;
    view.width = window->width;
    view.height = window->height;
    view.visible = true;
    view.enabled = control_enabled(window);
    view.focused = window->focused != 0;
    view.checked = control_checked(window);
    const MaskedField mask = masked_field(window);
    view.masked = mask.masked;
    view.text_length = mask.length;
    view.text = mask.masked ? "" : control_text(window);
    view.selected = -1;
    if (kind == DriverControlKind::list)
        fill_list(reinterpret_cast<ListBox*>(window), items, &view);
    if (kind == DriverControlKind::list || kind == DriverControlKind::slider) {
        view.scroll_up = named_part(window, "decbutton");
        view.scroll_down = named_part(window, "incbutton");
    }
    visit(context, owner, view);
}

/// Reports the buttons and sliders inside a list or a slider.
///
/// They are reported under `owner`, which is the list's or the slider's own
/// name, so a press can name the scroll button and that window together.
/// Labels, including a list's rows, are not reported.
///
/// @param window the list or the slider
/// @param owner the name the parts are listed under
/// @param context passed to `visit`
/// @param visit the visit
void walk_parts(Window* window, const char* owner, void* context, DriverControlVisitor visit) {
    if (window == nullptr || owner == nullptr)
        return;
    for (int32_t index = window->children.count - 1; index >= 0; --index) {
        Window* child = window->children.items[index];
        if (child == nullptr || !window_is_shown(child))
            continue;
        DriverControlKind kind{};
        const bool listed = classify(child, &kind);
        if (listed && (kind == DriverControlKind::button || kind == DriverControlKind::slider))
            report(child, kind, owner, context, visit);
        if (!listed || kind == DriverControlKind::slider)
            walk_parts(child, owner, context, visit);
    }
}

/// Walks one window's shown controls. A named window names the controls inside it.
///
/// @param window window to walk
/// @param owner the window name in force above it
/// @param context passed to `visit`
/// @param visit the visit
void walk(Window* window, const char* owner, void* context, DriverControlVisitor visit) {
    if (window == nullptr || !window_is_shown(window))
        return;
    DriverControlKind kind{};
    if (classify(window, &kind)) {
        report(window, kind, owner, context, visit);
        if (kind == DriverControlKind::list || kind == DriverControlKind::slider) {
            if (const char* part_owner = control_name(window); part_owner != nullptr)
                walk_parts(window, part_owner, context, visit);
        } else if (kind == DriverControlKind::label) {
            // A label that holds controls, such as a radio group with its
            // title and its radios, lists them under the same window.
            for (int32_t index = window->children.count - 1; index >= 0; --index)
                walk(window->children.items[index], owner, context, visit);
        }
        return;
    }
    if (const char* name = own_name(window); name != nullptr)
        owner = name;
    // Later children are drawn over earlier ones, so they are listed first.
    for (int32_t index = window->children.count - 1; index >= 0; --index)
        walk(window->children.items[index], owner, context, visit);
}

} // namespace

void desktop_visit_driver_controls(
    const Desktop* desktop, void* context, DriverControlVisitor visit
) {
    if (desktop == nullptr || visit == nullptr)
        return;
    for (int32_t index = desktop->windows.count - 1; index >= 0; --index) {
        Window* window = desktop->windows.items[index];
        const char* name = own_name(window);
        if (name == nullptr && window != nullptr && window->ops != nullptr)
            name = window->ops->class_name;
        walk(window, name != nullptr ? name : "", context, visit);
    }
}

} // namespace oa::boneyards
