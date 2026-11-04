// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The control tree: each control's parent, a dialog grouped over the screen
// it covers, an extension's window and a screen package's window, and the
// gadget under the pointer.
#include "oa/app/automation/tree.hpp"
#include "oa/app/automation/view.hpp"

#include "oa/test/check.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace {

namespace automation = oa::app::automation;

using oa::app::AutomationControl;
using oa::app::AutomationControlKind;
using automation::ControlListing;
using automation::ControlNode;
using automation::ControlParentKind;

/// A button of the listing, at a rectangle on the canvas.
///
/// @param name its name
/// @param x its left column
/// @param y its top row
/// @param width its width
/// @param height its height
/// @return the control, shown
AutomationControl button(const char* name, int32_t x, int32_t y, int32_t width, int32_t height) {
    AutomationControl control;
    control.name = name;
    control.kind = AutomationControlKind::button;
    control.x = x;
    control.y = y;
    control.width = width;
    control.height = height;
    control.visible = true;
    control.enabled = true;
    return control;
}

/// Finds a group's child by its name.
///
/// @param node the group
/// @param name the child's name: a group's parent name, or a leaf's control name
/// @return the child, or null when the group has none of that name
const ControlNode* child_named(const ControlNode& node, const std::string& name) {
    for (const ControlNode& child : node.children) {
        if (child.group && child.parent.name == name)
            return &child;
        if (!child.group && child.control.name == name)
            return &child;
    }
    return nullptr;
}

/// The four parents, and a dialog's controls grouped over the screen.
void check_parents_and_dialog() {
    AutomationControl ok = button("OK", 200, 180, 64, 20);
    ok.dialog = "msgbox";
    AutomationControl single = button("SINGLE", 16, 40, 96, 20);
    AutomationControl enter = button("ENTER", 8, 8, 40, 16);
    enter.window = "fixture";
    AutomationControl lamp = button("LAMP", 4, 4, 12, 12);
    lamp.window = "panel";
    lamp.package_window = true;

    const std::vector<AutomationControl> controls{enter, lamp, ok, single};
    const ControlListing listing = automation::control_listing(controls, "load_game", "OK");

    OA_CHECK(listing.flat.size() == 4);
    if (listing.flat.size() == 4) {
        OA_CHECK(listing.flat[0].control.name == "ENTER");
        OA_CHECK(listing.flat[0].parent.kind == ControlParentKind::extension_window);
        OA_CHECK(listing.flat[0].parent.name == "fixture");
        OA_CHECK(!listing.flat[0].hovered);
        OA_CHECK(listing.flat[1].parent.kind == ControlParentKind::window);
        OA_CHECK(listing.flat[1].parent.name == "panel");
        OA_CHECK(listing.flat[2].parent.kind == ControlParentKind::dialog);
        OA_CHECK(listing.flat[2].parent.name == "msgbox");
        OA_CHECK(listing.flat[2].hovered);
        OA_CHECK(listing.flat[3].parent.kind == ControlParentKind::screen_package);
        OA_CHECK(listing.flat[3].parent.name == "load_game");
    }

    // The extension's window is a root ahead of the screen. The package holds
    // its own window, then the dialog over the screen, then the screen's button.
    OA_CHECK(listing.tree.hovered == "OK");
    OA_CHECK(listing.tree.roots.size() == 2);
    if (listing.tree.roots.size() == 2) {
        const ControlNode& extension = listing.tree.roots[0];
        OA_CHECK(extension.group);
        OA_CHECK(extension.parent.kind == ControlParentKind::extension_window);
        OA_CHECK(extension.parent.name == "fixture");
        OA_CHECK(extension.children.size() == 1);
        if (extension.children.size() == 1) {
            OA_CHECK(!extension.children[0].group);
            OA_CHECK(extension.children[0].control.name == "ENTER");
            OA_CHECK(extension.children[0].parent.kind == ControlParentKind::extension_window);
        }

        const ControlNode& package = listing.tree.roots[1];
        OA_CHECK(package.group);
        OA_CHECK(package.parent.kind == ControlParentKind::screen_package);
        OA_CHECK(package.parent.name == "load_game");
        OA_CHECK(package.children.size() == 3);
        if (package.children.size() == 3) {
            const ControlNode& panel = package.children[0];
            OA_CHECK(panel.group && panel.parent.kind == ControlParentKind::window);
            OA_CHECK(panel.parent.name == "panel");
            OA_CHECK(panel.children.size() == 1 && !panel.children[0].group);
            if (panel.children.size() == 1)
                OA_CHECK(panel.children[0].control.name == "LAMP");

            const ControlNode& dialog = package.children[1];
            OA_CHECK(dialog.group && dialog.parent.kind == ControlParentKind::dialog);
            OA_CHECK(dialog.parent.name == "msgbox");
            const ControlNode* ok_node = child_named(dialog, "OK");
            OA_CHECK(ok_node != nullptr && ok_node->hovered);
            if (ok_node != nullptr) {
                OA_CHECK(ok_node->parent.kind == ControlParentKind::dialog);
                OA_CHECK(ok_node->parent.name == "msgbox");
                OA_CHECK(ok_node->control.dialog == "msgbox");
            }

            const ControlNode& own = package.children[2];
            OA_CHECK(!own.group && own.control.name == "SINGLE");
            OA_CHECK(own.parent.kind == ControlParentKind::screen_package);
            OA_CHECK(!own.hovered);
        }
    }
}

/// A dialog listed over a screen takes the pointer where their rectangles overlap.
void check_hovered_over_dialog() {
    AutomationControl ok = button("OK", 100, 100, 40, 20);
    ok.dialog = "yesorno";
    // The screen's own control fills the canvas, under the dialog.
    const AutomationControl background = button("SCREEN", 0, 0, 640, 480);
    const std::vector<AutomationControl> controls{ok, background};

    OA_CHECK(automation::hovered_control(controls, 110, 110, "") == "OK");
    OA_CHECK(automation::hovered_control(controls, 10, 10, "") == "SCREEN");
    OA_CHECK(automation::hovered_control(controls, -1, -1, "ATTACK") == "ATTACK");
    OA_CHECK(automation::hovered_control(controls, -1, -1, "").empty());

    const ControlListing over = automation::control_listing(controls, "skirmish", "OK");
    OA_CHECK(over.tree.roots.size() == 1);
    if (over.tree.roots.size() == 1) {
        const ControlNode* dialog = child_named(over.tree.roots[0], "yesorno");
        const ControlNode* screen = child_named(over.tree.roots[0], "SCREEN");
        OA_CHECK(dialog != nullptr && dialog->group);
        OA_CHECK(screen != nullptr && !screen->group);
        if (dialog != nullptr && screen != nullptr) {
            OA_CHECK(dialog->children.size() == 1 && dialog->children[0].hovered);
            OA_CHECK(!screen->hovered);
            OA_CHECK(screen->parent.name == "skirmish");
        }
    }
}

/// A label, a message box and a list row of the game's code page read as UTF-8.
void check_windows_1252_text() {
    AutomationControl label = button("MESSAGE", 40, 40, 200, 16);
    label.kind = AutomationControlKind::label;
    label.dialog = "msgbox";
    label.text = "User \x93Raptor9\x94 is not listed in this ladder.";
    AutomationControl rows = button("NAMES", 40, 80, 200, 64);
    rows.kind = AutomationControlKind::list;
    rows.items.push_back("caf\xE9");
    rows.items.push_back("caf\xC3\xA9");
    rows.item_keys.push_back("\x93");
    rows.item_keys.push_back("plain");
    AutomationControl secret = button("PASSWORD", 40, 160, 80, 16);
    secret.kind = AutomationControlKind::text_field;
    secret.masked = true;
    secret.text = "\x93secret";

    std::vector<AutomationControl> controls{label, rows, secret};
    automation::read_control_text(controls);
    OA_CHECK(
        controls[0].text == "User \xE2\x80\x9CRaptor9\xE2\x80\x9D is not listed in this ladder."
    );
    OA_CHECK(controls[0].text.find("Non-Unicode Text Error::") == std::string::npos);
    OA_CHECK(controls[1].items[0] == "caf\xC3\xA9");
    OA_CHECK(controls[1].items[1] == "caf\xC3\xA9");
    OA_CHECK(controls[1].item_keys[0] == "\xE2\x80\x9C");
    OA_CHECK(controls[1].item_keys[1] == "plain");
    OA_CHECK(controls[2].text == "\x93secret");

    const ControlListing listing = automation::control_listing(controls, "ladders", "");
    OA_CHECK(listing.flat.size() == 3);
    if (listing.flat.size() == 3) {
        OA_CHECK(listing.flat[0].parent.kind == ControlParentKind::dialog);
        OA_CHECK(
            listing.flat[0].control.text ==
            "User \xE2\x80\x9CRaptor9\xE2\x80\x9D is not listed in this ladder."
        );
        OA_CHECK(listing.flat[1].control.items[0] == "caf\xC3\xA9");
    }
}

/// A screen with no controls is still the package's node.
void check_empty_screen() {
    const ControlListing listing = automation::control_listing({}, "main_menu", "");
    OA_CHECK(listing.flat.empty());
    OA_CHECK(listing.tree.hovered.empty());
    OA_CHECK(listing.tree.roots.size() == 1);
    if (listing.tree.roots.size() == 1) {
        OA_CHECK(listing.tree.roots[0].group);
        OA_CHECK(listing.tree.roots[0].parent.kind == ControlParentKind::screen_package);
        OA_CHECK(listing.tree.roots[0].parent.name == "main_menu");
        OA_CHECK(listing.tree.roots[0].children.empty());
    }
}

} // namespace

int main() {
    check_parents_and_dialog();
    check_hovered_over_dialog();
    check_windows_1252_text();
    check_empty_screen();
    return oa::test::check_exit_status();
}
