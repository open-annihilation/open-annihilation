// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The controls of the screen shown (controls.hpp).
#include "controls.hpp"

#include "input.hpp"
#include "requests.hpp"

#include "oa/ui/frontend_multiplayer/panel.hpp"
#include "oa/ui/frontend_multiplayer/screens.hpp"
#include "oa/ui/frontend_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace oa::app::automation {
namespace {

namespace mp = oa::ui::frontend_multiplayer;

/// Tells whether a screen is one of the multiplayer screens.
///
/// @param screen the screen's registry id
/// @return true for the providers, TCP, game list, new game and battle room screens
bool multiplayer_screen(ScreenId screen) noexcept {
    return screen >= mp::kScreenProviders && screen <= mp::kScreenBattleroom;
}

/// Returns the name a dialog goes by: its GUI file's name without its
/// folder and suffix, in lower case, as the automation host names the
/// built-in screens' dialogs.
///
/// @param file the GUI file, as the panel's loader named it
/// @return the name, such as "tcp" for guis/tcp.gui
std::string dialog_name(std::string_view file) {
    if (const auto folder = file.find_last_of("/\\"); folder != std::string_view::npos)
        file.remove_prefix(folder + 1);
    if (const auto suffix = file.rfind('.'); suffix != std::string_view::npos)
        file = file.substr(0, suffix);
    std::string name(file);
    for (char& character : name)
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character - 'A' + 'a');
    return name;
}

/// Returns the kind of control a multiplayer control is.
///
/// @param control the control
/// @return its kind; nothing for the panel's root
std::optional<AutomationControlKind> multiplayer_kind(const mp::Control& control) noexcept {
    switch (control.type) {
    case mp::ControlType::button:
        if ((control.attributes & mp::kAttributeLabelText) != 0)
            return AutomationControlKind::label;
        return (control.attributes & mp::kAttributeCheckbox) != 0 ? AutomationControlKind::check_box
                                                                  : AutomationControlKind::button;
    case mp::ControlType::list_box:
        return AutomationControlKind::list;
    case mp::ControlType::text_box:
        return AutomationControlKind::text_field;
    case mp::ControlType::slider:
        return AutomationControlKind::slider;
    case mp::ControlType::label:
        return AutomationControlKind::label;
    case mp::ControlType::hot_surface:
        return AutomationControlKind::area;
    case mp::ControlType::image:
        return AutomationControlKind::image;
    case mp::ControlType::panel:
        return std::nullopt;
    }
    return std::nullopt;
}

/// Names a list's scroll arrows and adds them as controls a program can click.
///
/// The bar's own rectangle lies between its arrows, so a click at its centre
/// is not a click on an arrow. The arrows are `<bar>.up` and `<bar>.down`.
/// A list with no row pitch, a hidden bar or a bar with no arrows is left
/// with none: every row fits, or the bar is not a list's.
///
/// @param panel the panel shown
/// @param offset where the panel lies on the canvas
/// @param dialog the dialog's name; empty for the screen's own panel
/// @param[in,out] controls the panel's controls, which gain the arrows
void name_list_scroll(
    const mp::Panel& panel,
    mp::PanelOffset offset,
    const std::string& dialog,
    std::vector<AutomationControl>& controls
);

/// Collects the controls of the multiplayer screen shown, or of the dialog
/// over it: those of the panel that takes the pointer.
///
/// @param[out] controls receives the controls
void multiplayer_controls(std::vector<AutomationControl>& controls) {
    const mp::Panel& panel = mp::multiplayer_panel();
    if (panel.count <= 0)
        return;
    const mp::PanelOffset offset = mp::multiplayer_panel_offset();
    const std::string dialog =
        mp::multiplayer_modal() == &panel ? dialog_name(panel.name.data()) : std::string();
    for (int32_t index = 1; index < panel.count; ++index) {
        const mp::Control& source = panel.controls[static_cast<size_t>(index)];
        const auto kind = multiplayer_kind(source);
        if (!kind)
            continue;
        AutomationControl control;
        control.name = mp::control_name(source);
        control.kind = *kind;
        control.dialog = dialog;
        control.x = source.x + offset.x;
        control.y = source.y + offset.y;
        control.width = source.width;
        control.height = source.height;
        control.visible = source.active != 0;
        control.enabled = control.visible && !source.grayed && !source.display_only &&
                          (source.type != mp::ControlType::hot_surface || source.hot);
        control.focused = panel.focus == index;
        control.checked = *kind == AutomationControlKind::check_box && source.value != 0;
        const std::string_view text = mp::control_text(source);
        control.text =
            source.stages > 1
                ? std::string(oa::ui::frontend_renderer::staged_caption(text, source.stage))
                : std::string(text);
        if (*kind == AutomationControlKind::list) {
            control.items = source.items;
            if (source.item_keys.size() == source.items.size())
                control.item_keys = source.item_keys;
            control.first_visible = std::max<int32_t>(0, source.list_first);
            control.row_height =
                source.list_item_height > 0 ? source.list_item_height : panel.line_height;
            control.rows = mp::list_shown_rows(source, panel.line_height);
            control.selected =
                source.list_selection >= 0 &&
                        static_cast<size_t>(source.list_selection) < source.items.size()
                    ? source.list_selection
                    : -1;
        }
        controls.push_back(std::move(control));
    }
    name_list_scroll(panel, offset, dialog, controls);
}

void name_list_scroll(
    const mp::Panel& panel,
    mp::PanelOffset offset,
    const std::string& dialog,
    std::vector<AutomationControl>& controls
) {
    std::vector<std::string> added;
    for (int32_t index = 1; index < panel.count; ++index) {
        const mp::Control& source = panel.controls[static_cast<size_t>(index)];
        if (source.type != mp::ControlType::list_box || source.list_item_height <= 0)
            continue;
        const int32_t bar = mp::panel_group_member(panel, index, mp::ControlType::slider);
        if (bar == mp::kNoControl)
            continue;
        const mp::Control& slider = panel.controls[static_cast<size_t>(bar)];
        if (slider.active == 0 || slider.scroll.art == oa::ui::gui_input::ScrollArt::unbound)
            continue;
        const auto& back = slider.scroll.back_arrow;
        const auto& forward = slider.scroll.forward_arrow;
        if (back.width <= 0 || back.height <= 0 || forward.width <= 0 || forward.height <= 0)
            continue;
        const std::string bar_name(mp::control_name(slider));
        if (bar_name.empty())
            continue;
        const std::string up = bar_name + ".up";
        const std::string down = bar_name + ".down";
        const std::string list_name(mp::control_name(source));
        for (AutomationControl& listed : controls) {
            if (listed.kind == AutomationControlKind::list && listed.name == list_name) {
                listed.scroll_up = up;
                listed.scroll_down = down;
                break;
            }
        }
        const auto add = [&](const std::string& name,
                             int32_t arrow_x,
                             int32_t arrow_y,
                             int32_t arrow_width,
                             int32_t arrow_height) {
            if (std::find(added.begin(), added.end(), name) != added.end())
                return;
            added.push_back(name);
            AutomationControl arrow;
            arrow.name = name;
            arrow.kind = AutomationControlKind::button;
            arrow.dialog = dialog;
            arrow.x = arrow_x + offset.x;
            arrow.y = arrow_y + offset.y;
            arrow.width = arrow_width;
            arrow.height = arrow_height;
            arrow.visible = true;
            arrow.enabled = true;
            controls.push_back(std::move(arrow));
        };
        add(up, back.x, back.y, back.width, back.height);
        add(down, forward.x, forward.y, forward.width, forward.height);
    }
}

/// Writes a rectangle as the protocol does: [x, y, width, height].
///
/// @param[in,out] json the JSON being written
/// @param x its left column
/// @param y its top row
/// @param width its width
/// @param height its height
void write_rect(JsonWriter& json, int32_t x, int32_t y, int32_t width, int32_t height) {
    json.begin_array();
    json.integer(x);
    json.integer(y);
    json.integer(width);
    json.integer(height);
    json.end_array();
}

/// Writes a control's rectangle in the window's own pixels: the one the
/// host gives, else the canvas rectangle placed through the window.
///
/// @param[in,out] json the JSON being written
/// @param placement where the window shows the canvas
/// @param control the control
void write_window_rect(
    JsonWriter& json, const WindowPlacement& placement, const AutomationControl& control
) {
    if (control.window_pixels) {
        write_rect(
            json, control.window_x, control.window_y, control.window_width, control.window_height
        );
        return;
    }
    float left = 0.0F;
    float top = 0.0F;
    float right = 0.0F;
    float bottom = 0.0F;
    if (!canvas_to_window(
            placement, static_cast<float>(control.x), static_cast<float>(control.y), left, top
        ) ||
        !canvas_to_window(
            placement,
            static_cast<float>(control.x + control.width),
            static_cast<float>(control.y + control.height),
            right,
            bottom
        )) {
        json.null();
        return;
    }
    const auto pixel = [&placement](float coordinate) {
        return static_cast<int32_t>(std::lround(coordinate * placement.density));
    };
    write_rect(
        json, pixel(left), pixel(top), pixel(right) - pixel(left), pixel(bottom) - pixel(top)
    );
}

/// Collects the controls of the windows extensions show over the screen.
///
/// @param host the automation host
/// @param[out] controls replaced by those controls
void extension_windows(
    const oa::app::AutomationHost& host, std::vector<AutomationControl>& controls
) {
    controls.clear();
    if (host.windows != nullptr)
        host.windows(host.context, &controls);
}

} // namespace

void collect_controls(
    const CheckHost& check, const AutomationHost& host, std::vector<AutomationControl>& controls
) {
    controls.clear();
    if (multiplayer_screen(check.screen(check.context))) {
        if (mp::multiplayer_showing())
            multiplayer_controls(controls);
        // Open Annihilation's own screens over these, such as GET MAP in the
        // battle room, take the pointer first; the host lists them in the
        // window's pixels.
        std::vector<AutomationControl> own;
        if (host.controls != nullptr)
            host.controls(host.context, &own);
        controls.insert(controls.begin(), own.begin(), own.end());
    } else {
        host.controls(host.context, &controls);
        // A dialog's controls come first among the screen's: they take the
        // pointer over it. So do Open Annihilation's own screens' and the OA
        // button's, which the host places in the window's pixels and lists
        // where they belong.
        std::stable_partition(
            controls.begin(), controls.end(), [](const AutomationControl& control) {
                return !control.dialog.empty() || control.window_pixels;
            }
        );
    }
    // An extension's windows take the pointer over the screen and its dialogs.
    std::vector<AutomationControl> windows;
    extension_windows(host, windows);
    if (!windows.empty())
        controls.insert(controls.begin(), windows.begin(), windows.end());
}

void collect_controls(const Endpoint& endpoint, std::vector<AutomationControl>& controls) {
    collect_controls(endpoint.check_host(), endpoint.automation_host(), controls);
}

std::string_view control_kind_name(AutomationControlKind kind) noexcept {
    switch (kind) {
    case AutomationControlKind::button:
        return "button";
    case AutomationControlKind::check_box:
        return "check box";
    case AutomationControlKind::list:
        return "list";
    case AutomationControlKind::text_field:
        return "text field";
    case AutomationControlKind::slider:
        return "slider";
    case AutomationControlKind::label:
        return "label";
    case AutomationControlKind::area:
        return "area";
    case AutomationControlKind::image:
        return "image";
    }
    return "button";
}

std::vector<std::string> dialog_names(const std::vector<AutomationControl>& controls) {
    std::vector<std::string> dialogs;
    for (const AutomationControl& control : controls)
        if (!control.dialog.empty() &&
            std::find(dialogs.begin(), dialogs.end(), control.dialog) == dialogs.end())
            dialogs.push_back(control.dialog);
    return dialogs;
}

void write_dialogs_and_focus(JsonWriter& json, const std::vector<AutomationControl>& controls) {
    const AutomationControl* focused = nullptr;
    for (const AutomationControl& control : controls)
        if (control.focused && focused == nullptr)
            focused = &control;
    json.key("dialogs");
    json.begin_array();
    for (const std::string& dialog : dialog_names(controls))
        json.string(dialog);
    json.end_array();
    json.key("focused");
    if (focused != nullptr)
        json.string(focused->name);
    else
        json.null();
}

void answer_controls(Endpoint& endpoint, const Request& request, Answer& answer) {
    const CheckHost& host = endpoint.check_host();
    const std::string shown = screen_name(host.screen(host.context));
    if (const Json* screen = request.fields.find("screen");
        screen != nullptr && screen->type() != JsonType::null) {
        if (screen->string() == nullptr) {
            answer.refuse("bad_request", "screen names a screen", "screen");
            return;
        }
        if (*screen->string() != shown) {
            answer.refuse(
                "screen_changed", "the screen is " + shown + ", not " + *screen->string()
            );
            return;
        }
    }
    std::vector<AutomationControl> controls;
    collect_controls(endpoint, controls);
    const WindowPlacement placement = window_placement(endpoint);
    JsonWriter& json = answer.json;
    json.key("screen");
    json.string(shown);
    json.key("controls");
    json.begin_array();
    for (const AutomationControl& control : controls) {
        json.begin_object();
        json.key("name");
        json.string(control.name);
        json.key("kind");
        json.string(control_kind_name(control.kind));
        json.key("dialog");
        if (control.dialog.empty())
            json.null();
        else
            json.string(control.dialog);
        json.key("window");
        if (control.window.empty())
            json.null();
        else
            json.string(control.window);
        json.key("rect");
        write_rect(json, control.x, control.y, control.width, control.height);
        json.key("window_rect");
        write_window_rect(json, placement, control);
        json.key("enabled");
        json.boolean(control.enabled);
        json.key("visible");
        json.boolean(control.visible);
        json.key("text");
        json.string(control.text);
        json.key("focused");
        json.boolean(control.focused);
        if (control.kind == AutomationControlKind::check_box) {
            json.key("checked");
            json.boolean(control.checked);
        }
        if (control.kind == AutomationControlKind::list) {
            json.key("items");
            json.begin_array();
            for (const std::string& item : control.items)
                json.string(item);
            json.end_array();
            if (!control.item_keys.empty() && control.item_keys.size() == control.items.size()) {
                json.key("item_keys");
                json.begin_array();
                for (const std::string& key : control.item_keys)
                    json.string(key);
                json.end_array();
            }
            json.key("first_visible");
            json.integer(control.first_visible);
            json.key("rows");
            json.integer(control.rows);
            json.key("row_height");
            json.integer(control.row_height);
            json.key("selected");
            json.integer(control.selected);
            // Empty when every row fits, or the list has no scroll bar.
            // A multiplayer list names its bar's arrows.
            json.key("scroll_up");
            if (control.scroll_up.empty())
                json.null();
            else
                json.string(control.scroll_up);
            json.key("scroll_down");
            if (control.scroll_down.empty())
                json.null();
            else
                json.string(control.scroll_down);
        }
        json.end_object();
    }
    json.end_array();
}

} // namespace oa::app::automation
