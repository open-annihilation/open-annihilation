// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The screen, the tree and a control's centre (include/oa/app/automation/view.hpp).
#include "oa/app/automation/view.hpp"

#include "controls.hpp"
#include "input.hpp"

#include "oa/app/automation/screen.hpp"
#include "oa/app/app.hpp"
#include "oa/core/game_state.h"
#include "oa/formats/json.hpp"
#include "oa/ui/frontend_multiplayer/screens.hpp"

#include <cmath>
#include <string>
#include <vector>

namespace oa::app::automation {
namespace {

namespace json = oa::formats::json;
namespace mp = oa::ui::frontend_multiplayer;

/// Compares two names ignoring ASCII case.
///
/// @param one one name
/// @param other the other
/// @return true when they match aside from ASCII case
bool same_ascii(std::string_view one, std::string_view other) noexcept {
    if (one.size() != other.size())
        return false;
    for (std::size_t at = 0; at < one.size(); ++at) {
        unsigned char a = static_cast<unsigned char>(one[at]);
        unsigned char b = static_cast<unsigned char>(other[at]);
        if (a >= 'A' && a <= 'Z')
            a = static_cast<unsigned char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z')
            b = static_cast<unsigned char>(b - 'A' + 'a');
        if (a != b)
            return false;
    }
    return true;
}

/// The name a parent kind is given in the tree.
///
/// @param kind the kind
/// @return dialog, window, screen_package or extension_window
std::string_view parent_kind_name(ControlParentKind kind) noexcept {
    switch (kind) {
    case ControlParentKind::dialog:
        return "dialog";
    case ControlParentKind::window:
        return "window";
    case ControlParentKind::screen_package:
        return "screen_package";
    case ControlParentKind::extension_window:
        return "extension_window";
    }
    return "screen_package";
}

/// Writes a rectangle as the control list does: [x, y, width, height].
///
/// @param[in,out] writer the JSON being written
/// @param x its left column
/// @param y its top row
/// @param width its width
/// @param height its height
void write_rect(json::JsonWriter& writer, int32_t x, int32_t y, int32_t width, int32_t height) {
    writer.begin_array();
    writer.integer(x);
    writer.integer(y);
    writer.integer(width);
    writer.integer(height);
    writer.end_array();
}

/// Writes a control's rectangle in the window's pixels.
///
/// @param[in,out] writer the JSON being written
/// @param placement where the window shows the canvas
/// @param control the control
void write_window_rect(
    json::JsonWriter& writer, const WindowPlacement& placement, const AutomationControl& control
) {
    if (control.window_pixels) {
        write_rect(
            writer, control.window_x, control.window_y, control.window_width, control.window_height
        );
        return;
    }
    float left_edge = 0.0F;
    float top_edge = 0.0F;
    float right_edge = 0.0F;
    float bottom_edge = 0.0F;
    if (!canvas_to_window(
            placement,
            static_cast<float>(control.x),
            static_cast<float>(control.y),
            left_edge,
            top_edge
        ) ||
        !canvas_to_window(
            placement,
            static_cast<float>(control.x + control.width),
            static_cast<float>(control.y + control.height),
            right_edge,
            bottom_edge
        )) {
        writer.null();
        return;
    }
    const auto pixel = [&placement](float coordinate) {
        return static_cast<int32_t>(
            std::lround(coordinate * static_cast<double>(placement.density))
        );
    };
    const int32_t left_px = pixel(left_edge);
    const int32_t top_px = pixel(top_edge);
    write_rect(writer, left_px, top_px, pixel(right_edge) - left_px, pixel(bottom_edge) - top_px);
}

/// Writes one control's fields, the flat list's and its place in the tree.
///
/// @param[in,out] writer the JSON being written, an object already open
/// @param node the leaf
/// @param placement where the window shows the canvas
void write_leaf(
    json::JsonWriter& writer, const ControlNode& node, const WindowPlacement& placement
) {
    const AutomationControl& control = node.control;
    writer.key("kind");
    writer.string(control_kind_name(control.kind));
    writer.key("name");
    writer.string(control.name);
    writer.key("parent_kind");
    writer.string(parent_kind_name(node.parent.kind));
    writer.key("parent");
    writer.string(node.parent.name);
    writer.key("dialog");
    if (control.dialog.empty())
        writer.null();
    else
        writer.string(control.dialog);
    writer.key("window");
    if (control.window.empty())
        writer.null();
    else
        writer.string(control.window);
    writer.key("rect");
    write_rect(writer, control.x, control.y, control.width, control.height);
    writer.key("window_rect");
    write_window_rect(writer, placement, control);
    writer.key("enabled");
    writer.boolean(control.enabled);
    writer.key("visible");
    writer.boolean(control.visible);
    writer.key("text");
    if (control.masked)
        writer.string("");
    else
        writer.string(control.text);
    if (control.masked) {
        writer.key("length");
        writer.integer(control.text_length);
    }
    writer.key("focused");
    writer.boolean(control.focused);
    writer.key("hovered");
    writer.boolean(node.hovered);
    if (control.kind == AutomationControlKind::check_box) {
        writer.key("checked");
        writer.boolean(control.checked);
    }
    if (control.kind == AutomationControlKind::list) {
        writer.key("items");
        writer.begin_array();
        for (const std::string& item : control.items)
            writer.string(item);
        writer.end_array();
        if (!control.item_keys.empty() && control.item_keys.size() == control.items.size()) {
            writer.key("item_keys");
            writer.begin_array();
            for (const std::string& key : control.item_keys)
                writer.string(key);
            writer.end_array();
        }
        writer.key("first_visible");
        writer.integer(control.first_visible);
        writer.key("rows");
        writer.integer(control.rows);
        writer.key("row_height");
        writer.integer(control.row_height);
        writer.key("selected");
        writer.integer(control.selected);
        writer.key("scroll_up");
        if (control.scroll_up.empty())
            writer.null();
        else
            writer.string(control.scroll_up);
        writer.key("scroll_down");
        if (control.scroll_down.empty())
            writer.null();
        else
            writer.string(control.scroll_down);
    }
    write_control_access(writer, control);
}

/// Writes a tree node and its children.
///
/// @param[in,out] writer the JSON being written
/// @param node the node
/// @param placement where the window shows the canvas
void write_node(
    json::JsonWriter& writer, const ControlNode& node, const WindowPlacement& placement
) {
    writer.begin_object();
    writer.key("group");
    writer.boolean(node.group);
    if (node.group) {
        writer.key("kind");
        writer.string(parent_kind_name(node.parent.kind));
        writer.key("name");
        writer.string(node.parent.name);
        writer.key("children");
        writer.begin_array();
        for (const ControlNode& child : node.children)
            write_node(writer, child, placement);
        writer.end_array();
    } else {
        write_leaf(writer, node, placement);
    }
    writer.end_object();
}

/// The gadget the game itself has under the pointer.
///
/// @param automation the automation host
/// @return the name; empty when it names none
std::string game_hover(const AutomationHost& automation) {
    if (automation.hovered == nullptr)
        return {};
    const char* hover_name = automation.hovered(automation.context);
    return hover_name != nullptr ? std::string(hover_name) : std::string();
}

} // namespace

ControlListing listing_of(const CheckHost& check, const AutomationHost& automation) {
    std::vector<AutomationControl> controls;
    collect_controls(check, automation, controls);
    read_control_text(controls);
    const std::string shown = screen_name(check.screen(check.context));
    int32_t pointer_x = 0;
    int32_t pointer_y = 0;
    if (check.cursor != nullptr)
        check.cursor(check.context, &pointer_x, &pointer_y);
    const std::string under =
        hovered_control(controls, pointer_x, pointer_y, game_hover(automation));
    return control_listing(controls, shown, under);
}

std::string screen_object(const CheckHost& check, const AutomationHost& automation) {
    int32_t pointer_x = 0;
    int32_t pointer_y = 0;
    if (check.cursor != nullptr)
        check.cursor(check.context, &pointer_x, &pointer_y);
    std::vector<AutomationControl> controls;
    collect_controls(check, automation, controls);
    const std::string under =
        hovered_control(controls, pointer_x, pointer_y, game_hover(automation));
    json::JsonWriter writer;
    writer.begin_object();
    writer.key("screen");
    writer.string(screen_name(check.screen(check.context)));
    writer.key("frontend_state");
    writer.integer(check.frontend_state(check.context));
    write_dialogs_and_focus(writer, controls);
    writer.key("pointer");
    writer.begin_object();
    writer.key("x");
    writer.integer(pointer_x);
    writer.key("y");
    writer.integer(pointer_y);
    writer.end_object();
    const oa::Game* game =
        check.screen(check.context) == screen_id(Screen::match) && automation.match_game != nullptr
            ? automation.match_game(automation.context)
            : nullptr;
    writer.key("camera");
    if (game != nullptr) {
        writer.begin_object();
        writer.key("x");
        writer.integer(game->camera_x);
        writer.key("y");
        writer.integer(game->camera_y);
        writer.end_object();
    } else {
        writer.null();
    }
    writer.key("hovered");
    if (under.empty())
        writer.null();
    else
        writer.string(under);
    writer.end_object();
    return writer.text();
}

std::string tree_object(const CheckHost& check, const AutomationHost& automation) {
    const ControlListing listing = listing_of(check, automation);
    const WindowPlacement placement = window_placement(check);
    json::JsonWriter writer;
    writer.begin_object();
    writer.key("screen");
    writer.string(screen_name(check.screen(check.context)));
    writer.key("roots");
    writer.begin_array();
    for (const ControlNode& root : listing.tree.roots)
        write_node(writer, root, placement);
    writer.end_array();
    writer.end_object();
    return writer.text();
}

const ListedControl* find_named(const ControlListing& listing, std::string_view wanted) {
    for (const ListedControl& listed : listing.flat)
        if (listed.control.name == wanted)
            return &listed;
    for (const ListedControl& listed : listing.flat)
        if (same_ascii(listed.control.name, wanted))
            return &listed;
    return nullptr;
}

std::optional<ClickPoint> control_centre(const AutomationControl& control) {
    ClickPoint point;
    if (control.window_pixels) {
        if (control.window_width <= 0 || control.window_height <= 0)
            return std::nullopt;
        point.space = PointSpace::window;
        point.x = control.window_x + control.window_width / 2;
        point.y = control.window_y + control.window_height / 2;
        return point;
    }
    if (control.width <= 0 || control.height <= 0)
        return std::nullopt;
    point.space = PointSpace::game;
    point.x = control.x + control.width / 2;
    point.y = control.y + control.height / 2;
    return point;
}

std::optional<ClickPoint> list_row_point(const AutomationControl& row_list, int32_t row) {
    if (row_list.row_height <= 0 || row_list.rows <= 0)
        return std::nullopt;
    if (row < row_list.first_visible || row >= row_list.first_visible + row_list.rows)
        return std::nullopt;
    const int32_t within = row - row_list.first_visible;
    const int32_t local_y = within * row_list.row_height + row_list.row_height / 2;
    const int32_t local_x = row_list.width > 0 ? row_list.width / 2 : 0;
    ClickPoint point;
    if (row_list.window_pixels) {
        if (row_list.height <= 0 || row_list.window_width <= 0 || row_list.window_height <= 0)
            return std::nullopt;
        const int32_t span = row_list.width > 0 ? row_list.width : 1;
        point.space = PointSpace::window;
        point.x =
            row_list.window_x +
            static_cast<int32_t>((static_cast<int64_t>(local_x) * row_list.window_width) / span);
        point.y = row_list.window_y +
                  static_cast<int32_t>(
                      (static_cast<int64_t>(local_y) * row_list.window_height) / row_list.height
                  );
        return point;
    }
    point.space = PointSpace::game;
    point.x = row_list.x + local_x;
    point.y = row_list.y + local_y;
    return point;
}

void fill_observed(
    const CheckHost& check,
    const AutomationHost& automation,
    Observed& now,
    bool dialogs,
    bool read_focus
) {
    now = Observed{};
    now.focus_read = read_focus;
    now.screen = screen_name(check.screen(check.context));
    if (automation.match_world != nullptr)
        now.match = automation.match_world(automation.context);
    const ScreenId shown = check.screen(check.context);
    if (shown == mp::kScreenBattleroom && mp::multiplayer_lobby().game != nullptr)
        now.room = &mp::multiplayer_lobby();
    // The tree is walked only for a frame that names its dialogs or reads
    // the focus. A launcher's status line does neither on an ordinary frame.
    if (!dialogs && !read_focus)
        return;
    std::vector<AutomationControl> controls;
    collect_controls(check, automation, controls);
    if (dialogs)
        now.dialogs = dialog_names(controls);
    if (!read_focus)
        return;
    read_shown_focus(now, controls);
}

} // namespace oa::app::automation
