// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The control listing both servers share: the flat list the automation
// endpoint answers with, and beside it a tree. Each control's parent is the
// dialog, the window a screen package reported, the screen package, or the
// extension window it belongs to. The tree is not part of a --fark answer.
#pragma once

#include "oa/app/automation_host.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app::automation {

/// What a control belongs to in the tree.
enum class ControlParentKind : uint8_t {
    dialog,           ///< a dialog over the screen
    window,           ///< a window a screen package reported
    screen_package,   ///< the screen package shown
    extension_window, ///< a window an extension shows over the screen
};

/// One control's parent: its kind and the name the listing gives it.
struct ControlParent {
    ControlParentKind kind{}; ///< which kind of parent
    std::string name;         ///< the dialog, window, package or extension window
};

/// Returns the parent a control belongs to.
///
/// A control whose `window` is set belongs to that window: a screen
/// package's window when `package_window` is set, otherwise an extension's
/// window. Otherwise a control whose `dialog` is set belongs to that
/// dialog. Every other control belongs to the screen package.
///
/// @param control the control
/// @param package the screen package shown, by the name `screen` gives it
/// @return the parent
[[nodiscard]] ControlParent
control_parent(const AutomationControl& control, std::string_view package);

/// One node of the control tree: a group (a parent) or one control.
struct ControlNode {
    /// A group is a parent. A leaf is one control.
    bool group{};
    /// A group's own kind and name, or a leaf's parent.
    ControlParent parent{};
    /// A leaf's control. A group leaves it empty.
    AutomationControl control{};
    /// The leaf is the hovered gadget. A group is never hovered.
    bool hovered{};
    /// A group's children, in the flat list's order: nested groups and controls.
    std::vector<ControlNode> children;
};

/// The tree beside the flat list.
struct ControlTree {
    /// The extension windows, in the flat list's order, then the screen package.
    std::vector<ControlNode> roots;
    /// The hovered gadget's name; empty when none is.
    std::string hovered;
};

/// One control of the flat list, with the parent the tree gives it.
struct ListedControl {
    AutomationControl control{}; ///< the control, as the flat list holds it
    ControlParent parent{};      ///< the dialog, window, package or extension window
    bool hovered{};              ///< this control is the hovered gadget
};

/// The flat list and the tree of one screen.
struct ControlListing {
    std::vector<ListedControl> flat; ///< the controls, in the order `controls` answers
    ControlTree tree;                ///< the same controls, under their parents
};

/// Builds the tree of a flat control list.
///
/// Extension windows are roots, in the order their controls appear. The
/// screen package is a root after them, and is present even when it holds
/// nothing. A dialog and a window the package reported are groups under the
/// package, in the order their first control appears, ahead of the package's
/// own controls when the flat list lists them first. A leaf whose name is
/// `hovered` is the hovered gadget.
///
/// @param controls the flat list, in the order `controls` answers
/// @param package the screen package shown, by the name `screen` gives it
/// @param hovered the hovered gadget's name; empty for none
/// @return the tree
[[nodiscard]] ControlTree control_tree(
    const std::vector<AutomationControl>& controls,
    std::string_view package,
    std::string_view hovered
);

/// Builds the flat list with each control's parent, and the tree beside it.
///
/// @param controls the flat list, in the order `controls` answers
/// @param package the screen package shown, by the name `screen` gives it
/// @param hovered the hovered gadget's name; empty for none
/// @return the listing
[[nodiscard]] ControlListing control_listing(
    const std::vector<AutomationControl>& controls,
    std::string_view package,
    std::string_view hovered
);

/// Returns the name of the gadget under the pointer.
///
/// The first visible control of the flat list whose rectangle holds the
/// point is that gadget: the list is in the order controls take the pointer,
/// an extension's window before a dialog, and a dialog before the screen.
/// When no listed control holds the point, the name is the gadget the game
/// itself has under the pointer.
///
/// @param controls the flat list, in the order `controls` answers
/// @param x the pointer's column on the canvas
/// @param y the pointer's row on the canvas
/// @param game_hover the gadget the game has under the pointer; empty for none
/// @return the name; empty when neither names one
[[nodiscard]] std::string hovered_control(
    const std::vector<AutomationControl>& controls,
    int32_t x,
    int32_t y,
    std::string_view game_hover
);

} // namespace oa::app::automation
