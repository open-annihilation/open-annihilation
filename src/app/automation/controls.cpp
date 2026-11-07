// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The controls of the screen shown (controls.hpp).
#include "controls.hpp"

#include "input.hpp"
#include "requests.hpp"

#include "oa/app/control_names.hpp"
#include "oa/ui/frontend_multiplayer/panel.hpp"
#include "oa/ui/frontend_multiplayer/screens.hpp"
#include "oa/ui/frontend_renderer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
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

/// Reports whether two names are the same, ignoring ASCII case.
bool same_control_name(std::string_view left, std::string_view right) {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        unsigned char a = static_cast<unsigned char>(left[index]);
        unsigned char b = static_cast<unsigned char>(right[index]);
        if (a >= 'A' && a <= 'Z')
            a = static_cast<unsigned char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z')
            b = static_cast<unsigned char>(b - 'A' + 'a');
        if (a != b)
            return false;
    }
    return true;
}

/// Reports whether `name` begins with `prefix`, ignoring ASCII case.
bool prefixed(std::string_view name, std::string_view prefix) {
    if (name.size() < prefix.size())
        return false;
    return same_control_name(name.substr(0, prefix.size()), prefix);
}

/// The letter a quick key names, A in slot 0. Anything else is -1.
int letter_slot(int quick_key) {
    if (quick_key >= 'a' && quick_key <= 'z')
        return quick_key - 'a';
    if (quick_key >= 'A' && quick_key <= 'Z')
        return quick_key - 'A';
    return -1;
}

/// The player a slot's name ends with: LOGO0 is player 1.
int slot_player_number(std::string_view name) {
    if (name.empty())
        return 0;
    std::size_t start = name.size();
    while (start > 0 && name[start - 1] >= '0' && name[start - 1] <= '9')
        --start;
    if (start == name.size())
        return 0;
    int number = 0;
    for (std::size_t index = start; index < name.size(); ++index)
        number = number * 10 + (name[index] - '0');
    return number + 1;
}

/// Fills a battle-room control's label, value and shortcut from the names table.
///
/// The namer is installed with the game. A test that collects these screens
/// without it keeps the labels the captions already gave. This collector does
/// not call the names table itself: the providers, the game list and the
/// battle room link without it, and a letter on those screens does nothing
/// but type. Only a yes-or-no question's first live button for a letter is
/// a shortcut.
///
/// @param panel the panel shown
/// @param source the panel's control
/// @param index `source`'s place in the panel
/// @param questions the panel is a yes-or-no question
/// @param first_letter the first live button that names each letter; -1 for none
/// @param[in,out] control the control just collected
void speak_multiplayer(
    const mp::Panel& panel,
    const mp::Control& source,
    int32_t index,
    bool questions,
    const std::array<int, 26>& first_letter,
    AutomationControl& control
) {
    const oa::app::ControlNamer namer = oa::app::control_namer();
    if (namer == nullptr)
        return;
    if (control.kind == oa::app::AutomationControlKind::slider)
        oa::app::fill_scroll_range(control, source.scroll.range, source.scroll.knob, {});
    oa::app::ControlReading reading;
    reading.gui.assign(panel.name.data());
    reading.quick_key = source.quick_key;
    reading.stages = source.stages;
    reading.player = slot_player_number(control.name);
    if (prefixed(control.name, "LOGO") && source.stage < 10)
        reading.colour = source.stage;
    if (prefixed(control.name, "TEAMICONS"))
        reading.alliance_frame = source.stage;
    if (same_control_name(control.name, "battlestart")) {
        reading.doors_known = true;
        reading.doors_closed = source.hot;
    }
    if (prefixed(control.name, "SIDE")) {
        if (source.stage == 0)
            reading.side_name = "Arm";
        else if (source.stage == 1)
            reading.side_name = "Core";
        else if (source.stage == 2)
            reading.side_name = "Watcher";
    }
    reading.neighbours.reserve(static_cast<std::size_t>(std::max<int32_t>(panel.count - 1, 0)));
    for (int32_t other_index = 1; other_index < panel.count; ++other_index) {
        const mp::Control& other = panel.controls[static_cast<std::size_t>(other_index)];
        const std::string_view other_name = mp::control_name(other);
        reading.neighbours.emplace_back(other_name, mp::control_text(other));
        if (other.type == mp::ControlType::label &&
            same_control_name(other.link.data(), control.name)) {
            const std::string_view linked = mp::control_text(other);
            if (!linked.empty())
                reading.linked_label.assign(linked);
        }
    }
    if (source.type == mp::ControlType::slider) {
        reading.has_slider = true;
        reading.knob = source.scroll.knob;
        reading.range = source.scroll.range;
    }
    if (questions && source.type == mp::ControlType::button && source.active != 0 &&
        !source.grayed) {
        const int slot = letter_slot(source.quick_key);
        if (slot >= 0 && first_letter[static_cast<std::size_t>(slot)] == index)
            reading.publish_letter = true;
    }
    namer(control, reading);
}

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
    // A letter answers only a yes-or-no question, and only its first live
    // button. On the other multiplayer screens it types into a line.
    const bool questions = dialog_name(panel.name.data()) == "yesorno";
    std::array<int, 26> first_letter{};
    first_letter.fill(-1);
    if (questions) {
        for (int32_t index = 1; index < panel.count; ++index) {
            const mp::Control& source = panel.controls[static_cast<std::size_t>(index)];
            if (source.type != mp::ControlType::button || source.active == 0 || source.grayed)
                continue;
            const int slot = letter_slot(source.quick_key);
            if (slot >= 0 && first_letter[static_cast<std::size_t>(slot)] < 0)
                first_letter[static_cast<std::size_t>(slot)] = static_cast<int>(index);
        }
    }
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
        speak_multiplayer(panel, source, index, questions, first_letter, control);
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
    // A screen package's own windows come next: over the screen, under an
    // extension's.
    std::vector<AutomationControl> windows;
    extension_windows(host, windows);
    if (!windows.empty())
        controls.insert(controls.begin(), windows.begin(), windows.end());
    std::vector<AutomationControl> package;
    if (host.package_windows != nullptr)
        host.package_windows(host.context, &package);
    if (!package.empty()) {
        auto place = controls.begin();
        while (place != controls.end() && !place->window.empty() && !place->package_window)
            ++place;
        controls.insert(place, package.begin(), package.end());
    }
    // A screen button an extension window takes is the same press twice.
    // The window keeps it: its button, or the rectangle where its art takes
    // the press. The screen button stays listed, hidden, and stays enabled.
    conceal_screen_buttons_under_extension(controls);
    // A modal cover hides extension and package windows. The screen's own
    // controls, and a screen drawn over the cover, stay as the host listed them.
    conceal_windows_behind_modal(controls);
    for (AutomationControl& control : controls)
        finish_control_access(control);
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

void write_control_access(JsonWriter& json, const AutomationControl& control) {
    json.key("role");
    json.string(control_role_name(control.role));
    json.key("label");
    json.string(control.label);
    if (!control.description.empty()) {
        json.key("description");
        json.string(control.description);
    }
    if (!control.value_text.empty()) {
        json.key("value_text");
        json.string(control.value_text);
    }
    if (control.has_value) {
        json.key("value");
        json.integer(control.value);
    }
    if (control.has_range) {
        json.key("min");
        json.integer(control.range_least);
        json.key("max");
        json.integer(control.range_most);
        json.key("step");
        json.integer(control.step);
    }
    if (control.place > 0) {
        json.key("position");
        json.integer(control.place);
    }
    if (control.set_size > 0) {
        json.key("set_size");
        json.integer(control.set_size);
    }
    if (!control.shortcut.empty()) {
        json.key("shortcut");
        json.string(control.shortcut);
    }
    if (control.live != AutomationLive::off) {
        json.key("live");
        json.string(live_name(control.live));
    }
    json.key("hidden");
    json.boolean(control.hidden);
    json.key("expanded");
    json.boolean(control.expanded);
    json.key("pressed");
    json.boolean(control.pressed);
    json.key("modal");
    json.boolean(control.modal);
    // A list already wrote its integer selected. The boolean is the other controls'.
    if (control.kind != AutomationControlKind::list) {
        json.key("selected");
        json.boolean(control.chosen);
    }
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
    for (const AutomationControl& control : controls) {
        // A hidden control stays in the tree. The focus the screen names is
        // the first one that is shown, so a kit screen's control wins over a
        // hidden control underneath it.
        if (!control.focused || control.hidden || focused != nullptr)
            continue;
        focused = &control;
    }
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
    json.key("focus_shown");
    json.boolean(focused != nullptr && focused->focus_shown);
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
        if (control.masked)
            json.string("");
        else
            json.string(control.text);
        if (control.masked) {
            json.key("length");
            json.integer(control.text_length);
        }
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
        write_control_access(json, control);
        json.end_object();
    }
    json.end_array();
}

} // namespace oa::app::automation
