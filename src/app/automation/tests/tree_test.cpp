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
#include <string_view>
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

/// A scroll bar reports its range, and a control behind a modal stays listed, hidden.
void check_slider_and_hidden() {
    AutomationControl bar = button("SLIDER", 10, 10, 12, 80);
    bar.kind = AutomationControlKind::slider;
    bar.text = "keep";
    oa::app::fill_scroll_range(bar, 10, 3, "Speed");
    OA_CHECK(bar.role == oa::app::AutomationControlRole::slider);
    OA_CHECK(bar.has_value && bar.value == 3);
    OA_CHECK(bar.has_range && bar.range_least == 0 && bar.range_most == 9 && bar.step == 1);
    OA_CHECK(bar.value_text == "Speed 3");
    OA_CHECK(bar.label == "Speed");
    OA_CHECK(bar.text == "keep");

    AutomationControl ok = button("OK", 200, 180, 64, 20);
    ok.modal = true;
    AutomationControl cancel = button("CANCEL", 16, 40, 96, 20);
    std::vector<AutomationControl> covered{ok, cancel};
    oa::app::conceal_behind_modal(covered);
    OA_CHECK(covered.size() == 2);
    if (covered.size() == 2) {
        OA_CHECK(covered[0].name == "OK" && covered[0].enabled && !covered[0].hidden);
        OA_CHECK(covered[1].name == "CANCEL" && covered[1].hidden && !covered[1].enabled);
    }

    AutomationControl secret = button("PASSWORD", 40, 160, 80, 16);
    secret.kind = AutomationControlKind::text_field;
    secret.masked = true;
    secret.text = "ridge-secret";
    secret.label = secret.text;
    oa::app::finish_control_access(secret);
    OA_CHECK(secret.label.empty());
    OA_CHECK(secret.role == oa::app::AutomationControlRole::password);
    OA_CHECK(secret.text == "ridge-secret");

    AutomationControl field = button("GAMENAME", 40, 80, 160, 16);
    field.kind = AutomationControlKind::text_field;
    field.text = "ridge";
    oa::app::finish_control_access(field);
    OA_CHECK(field.label.empty());
    OA_CHECK(field.text == "ridge");
    OA_CHECK(field.role == oa::app::AutomationControlRole::text_field);
}

/// One focus event when the focused control changes, and none when it stays.
void check_focus_changes_once() {
    struct Kept {
        std::vector<std::string> events;
    };

    Kept kept;
    automation::EventHooks hooks;
    hooks.context = &kept;
    hooks.wanted = [](void*, uint32_t) { return true; };
    hooks.begin = [](void*, std::string_view kind) {
        automation::JsonWriter event;
        event.begin_object();
        event.key("event");
        event.string(kind);
        return event;
    };
    hooks.send = [](void* context, automation::JsonWriter& event) {
        event.end_object();
        static_cast<Kept*>(context)->events.push_back(event.text());
    };

    automation::EventWatch watch;
    automation::Observed now;
    now.screen = "main_menu";
    now.focus_name = "search";
    now.focus_label = "Search";
    now.focus_role = "search";
    automation::watch_events(watch, now, hooks);
    OA_CHECK(kept.events.empty());

    now.focus_name = "close";
    now.focus_label = "Close";
    now.focus_role = "button";
    automation::watch_events(watch, now, hooks);
    OA_CHECK(kept.events.size() == 1);
    if (kept.events.size() == 1) {
        const std::string& sent = kept.events[0];
        OA_CHECK(sent.find("\"event\":\"focus\"") != std::string::npos);
        OA_CHECK(sent.find("\"name\":\"close\"") != std::string::npos);
        OA_CHECK(sent.find("\"label\":\"Close\"") != std::string::npos);
        OA_CHECK(sent.find("\"role\":\"button\"") != std::string::npos);
    }

    automation::watch_events(watch, now, hooks);
    OA_CHECK(kept.events.size() == 1);
}

/// A frame that does not read the focus, then one that does, sends nothing.
void check_focus_reread_sends_nothing() {
    struct Kept {
        std::vector<std::string> events;
    };

    Kept kept;
    automation::EventHooks hooks;
    hooks.context = &kept;
    hooks.wanted = [](void*, uint32_t) { return true; };
    hooks.begin = [](void*, std::string_view kind) {
        automation::JsonWriter event;
        event.begin_object();
        event.key("event");
        event.string(kind);
        return event;
    };
    hooks.send = [](void* context, automation::JsonWriter& event) {
        event.end_object();
        static_cast<Kept*>(context)->events.push_back(event.text());
    };

    automation::EventWatch watch;
    automation::Observed now;
    now.screen = "main_menu";
    now.focus_name = "search";
    automation::watch_events(watch, now, hooks);
    OA_CHECK(kept.events.empty());

    now.focus_read = false;
    now.focus_name = "close";
    automation::watch_events(watch, now, hooks);
    OA_CHECK(kept.events.empty());

    now.focus_read = true;
    automation::watch_events(watch, now, hooks);
    OA_CHECK(kept.events.empty());

    now.focus_name = "search";
    automation::watch_events(watch, now, hooks);
    OA_CHECK(kept.events.size() == 1);
}

/// The focus is the first focused control that is not hidden.
void check_focus_skips_hidden() {
    AutomationControl hidden = button("MULTI", 16, 40, 96, 20);
    hidden.focused = true;
    hidden.hidden = true;
    AutomationControl modal = button("OK", 200, 180, 64, 20);
    modal.focused = true;
    modal.modal = true;
    modal.text = "OK";
    oa::app::finish_control_access(modal);

    const std::vector<AutomationControl> covered_controls{hidden, modal};
    automation::Observed covered;
    automation::read_shown_focus(covered, covered_controls);
    OA_CHECK(covered.focus_name == "OK");
    OA_CHECK(covered.focus_label == "OK");
    OA_CHECK(covered.focus_role == "button");

    const std::vector<AutomationControl> hidden_only{hidden};
    automation::Observed alone;
    automation::read_shown_focus(alone, hidden_only);
    OA_CHECK(alone.focus_name.empty());
    alone.screen = "main_menu";

    struct Kept {
        std::vector<std::string> events;
    };

    Kept kept;
    automation::EventHooks hooks;
    hooks.context = &kept;
    hooks.wanted = [](void*, uint32_t) { return true; };
    hooks.begin = [](void*, std::string_view kind) {
        automation::JsonWriter event;
        event.begin_object();
        event.key("event");
        event.string(kind);
        return event;
    };
    hooks.send = [](void* context, automation::JsonWriter& event) {
        event.end_object();
        static_cast<Kept*>(context)->events.push_back(event.text());
    };

    automation::EventWatch watch;
    automation::Observed shown;
    shown.screen = "main_menu";
    shown.focus_name = "SINGLE";
    automation::watch_events(watch, shown, hooks);
    OA_CHECK(kept.events.empty());
    automation::watch_events(watch, alone, hooks);
    OA_CHECK(kept.events.size() == 1);
    if (kept.events.size() == 1) {
        OA_CHECK(kept.events[0].find("\"event\":\"focus\"") != std::string::npos);
        OA_CHECK(kept.events[0].find("\"name\":null") != std::string::npos);
    }
}

/// A screen button an extension button covers is listed hidden, still enabled.
void check_extension_cover() {
    auto covered = [](const std::vector<AutomationControl>& controls, const char* name) {
        for (const AutomationControl& control : controls) {
            if (control.name == name)
                return control.hidden;
        }
        return false;
    };
    auto still_enabled = [](const std::vector<AutomationControl>& controls, const char* name) {
        for (const AutomationControl& control : controls) {
            if (control.name == name)
                return control.enabled;
        }
        return false;
    };

    AutomationControl same_cover = button("SingleBtn", 82, 393, 96, 20);
    same_cover.window = "MainMenuOverlay";
    AutomationControl same = button("SINGLE", 82, 393, 96, 20);

    // The screen button sits inside a cover of a close size.
    AutomationControl inner_cover = button("MultiBtn", 70, 420, 120, 40);
    inner_cover.window = "MainMenuOverlay";
    AutomationControl inner = button("MULTI", 82, 430, 96, 20);

    // The cover sits inside a screen button of a close size.
    AutomationControl around_cover = button("IntroBtn", 220, 104, 40, 12);
    around_cover.window = "MainMenuOverlay";
    AutomationControl around = button("INTRO", 200, 100, 96, 20);

    // A cover more than four times the button does not hide it.
    AutomationControl huge = button("Map", 0, 0, 640, 480);
    huge.window = "MapPanel";
    AutomationControl planet = button("PLANET", 300, 300, 20, 20);

    AutomationControl package_cover = button("PkgBtn", 400, 10, 50, 20);
    package_cover.window = "panel";
    package_cover.package_window = true;
    AutomationControl under_package = button("UNDER", 400, 10, 50, 20);

    AutomationControl pixel_cover = button("PixCover", 10, 10, 40, 20);
    pixel_cover.window = "Overlay";
    AutomationControl pixel_screen = button("PIX", 10, 10, 40, 20);
    pixel_screen.window_pixels = true;

    AutomationControl pixel_button = button("CanCover", 500, 10, 40, 20);
    pixel_button.window = "oa.settings";
    pixel_button.window_pixels = true;
    AutomationControl canvas = button("CANVAS", 500, 10, 40, 20);

    AutomationControl gray_cover = button("GrayCover", 10, 200, 40, 20);
    gray_cover.window = "MainMenuOverlay";
    AutomationControl gray = button("GRAY", 10, 200, 40, 20);
    gray.enabled = false;

    std::vector<AutomationControl> controls{
        same_cover,
        same,
        inner_cover,
        inner,
        around_cover,
        around,
        huge,
        planet,
        package_cover,
        under_package,
        pixel_cover,
        pixel_screen,
        pixel_button,
        canvas,
        gray_cover,
        gray,
    };
    oa::app::conceal_screen_buttons_under_extension(controls);

    OA_CHECK(covered(controls, "SINGLE") && still_enabled(controls, "SINGLE"));
    OA_CHECK(covered(controls, "MULTI") && still_enabled(controls, "MULTI"));
    OA_CHECK(covered(controls, "INTRO") && still_enabled(controls, "INTRO"));
    OA_CHECK(!covered(controls, "PLANET") && still_enabled(controls, "PLANET"));
    OA_CHECK(!covered(controls, "UNDER") && still_enabled(controls, "UNDER"));
    OA_CHECK(!covered(controls, "PIX") && still_enabled(controls, "PIX"));
    OA_CHECK(!covered(controls, "CANVAS") && still_enabled(controls, "CANVAS"));
    OA_CHECK(covered(controls, "GRAY") && !still_enabled(controls, "GRAY"));
    OA_CHECK(!covered(controls, "SingleBtn"));

    for (const AutomationControl& control : controls) {
        if (control.name == "SINGLE") {
            OA_CHECK(control.x == 82 && control.y == 393);
            OA_CHECK(control.width == 96 && control.height == 20);
        }
    }

    // A hidden, enabled cover still takes the press. It sits inside a screen
    // button of a close size, so that button is listed hidden and stays enabled.
    AutomationControl former = button("SingleFormer", 179, 393, 56, 20);
    former.window = "MainMenuOverlay";
    former.hidden = true;
    AutomationControl under_former = button("SINGLE", 139, 393, 96, 20);

    // A hidden, disabled cover does not take the press, so it hides nothing.
    AutomationControl locked = button("LockedPlanet", 179, 200, 56, 20);
    locked.window = "MapPanel";
    locked.hidden = true;
    locked.enabled = false;
    AutomationControl under_locked = button("LOCKED", 139, 200, 96, 20);

    std::vector<AutomationControl> pressed{former, under_former, locked, under_locked};
    oa::app::conceal_screen_buttons_under_extension(pressed);
    OA_CHECK(covered(pressed, "SINGLE") && still_enabled(pressed, "SINGLE"));
    OA_CHECK(!covered(pressed, "LOCKED") && still_enabled(pressed, "LOCKED"));
}

/// A window's press rectangle hides a screen button wholly inside it.
///
/// The rectangle is far larger than the button, so the close-size rule would
/// leave the button listed. A button the rectangle does not reach stays listed.
void check_window_press_area() {
    auto covered = [](const std::vector<AutomationControl>& controls, const char* name) {
        for (const AutomationControl& control : controls) {
            if (control.name == name)
                return control.hidden;
        }
        return false;
    };
    auto still_enabled = [](const std::vector<AutomationControl>& controls, const char* name) {
        for (const AutomationControl& control : controls) {
            if (control.name == name)
                return control.enabled;
        }
        return false;
    };

    // The window's own button sits at the right. Its art takes the press over
    // the whole panel, about forty times this EXIT.
    AutomationControl strip_button = button("ExitBtn", 464, 430, 96, 20);
    strip_button.window = "MainMenuOverlay";
    strip_button.window_press_x = 0;
    strip_button.window_press_y = 378;
    strip_button.window_press_width = 640;
    strip_button.window_press_height = 102;
    AutomationControl under_art = button("EXIT", 8, 452, 80, 20);

    // Above the panel: the rectangle does not reach it.
    AutomationControl above = button("SINGLE", 82, 200, 96, 20);

    // Overlaps the panel's top edge, so it is not wholly inside.
    AutomationControl overlap = button("EDGE", 8, 360, 80, 30);

    AutomationControl gray = button("GRAY", 100, 452, 80, 20);
    gray.enabled = false;

    std::vector<AutomationControl> controls{strip_button, under_art, above, overlap, gray};
    oa::app::conceal_screen_buttons_under_extension(controls);

    OA_CHECK(covered(controls, "EXIT") && still_enabled(controls, "EXIT"));
    OA_CHECK(!covered(controls, "SINGLE") && still_enabled(controls, "SINGLE"));
    OA_CHECK(!covered(controls, "EDGE") && still_enabled(controls, "EDGE"));
    OA_CHECK(!covered(controls, "ExitBtn") && still_enabled(controls, "ExitBtn"));
    OA_CHECK(covered(controls, "GRAY") && !still_enabled(controls, "GRAY"));
    for (const AutomationControl& control : controls) {
        if (control.name == "EXIT") {
            OA_CHECK(control.x == 8 && control.y == 452);
            OA_CHECK(control.width == 80 && control.height == 20);
        }
    }

    // A package window's rectangle covers nothing of the screen.
    AutomationControl package_button = button("Pkg", 0, 378, 40, 20);
    package_button.window = "panel";
    package_button.package_window = true;
    package_button.window_press_x = 0;
    package_button.window_press_y = 378;
    package_button.window_press_width = 640;
    package_button.window_press_height = 102;
    AutomationControl under_package = button("PKGEXIT", 8, 452, 80, 20);
    std::vector<AutomationControl> packaged{package_button, under_package};
    oa::app::conceal_screen_buttons_under_extension(packaged);
    OA_CHECK(!covered(packaged, "PKGEXIT") && still_enabled(packaged, "PKGEXIT"));

    // A window that states no rectangle leaves a button it does not sit on.
    AutomationControl no_area = button("Plain", 10, 10, 20, 20);
    no_area.window = "Overlay";
    no_area.window_press_x = 0;
    no_area.window_press_y = 378;
    AutomationControl left_alone = button("ALONE", 8, 452, 80, 20);
    std::vector<AutomationControl> unstated{no_area, left_alone};
    oa::app::conceal_screen_buttons_under_extension(unstated);
    OA_CHECK(!covered(unstated, "ALONE") && still_enabled(unstated, "ALONE"));
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
    check_slider_and_hidden();
    check_focus_changes_once();
    check_focus_reread_sends_nothing();
    check_focus_skips_hidden();
    check_extension_cover();
    check_window_press_area();
    check_empty_screen();
    return oa::test::check_exit_status();
}
