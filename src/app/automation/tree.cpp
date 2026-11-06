// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The control tree (include/oa/app/automation/tree.hpp).
#include "oa/app/automation/tree.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app::automation {
namespace {

/// One node while the tree is built. Children are indexes into the same list,
/// so a node moving as the list grows does not leave a dangling child.
struct BuildNode {
    bool group{};
    ControlParent parent{};
    AutomationControl control{};
    bool hovered{};
    std::vector<std::size_t> children;
};

/// The kind and name that tell one group from another.
struct GroupKey {
    ControlParentKind kind{};
    std::string name;
};

/// Tells whether two group keys name the same group.
///
/// @param left one key
/// @param right the other
/// @return true when the kind and the name are equal
bool same_group(const GroupKey& left, const GroupKey& right) {
    return left.kind == right.kind && left.name == right.name;
}

/// Tells whether a control's rectangle holds a point on the canvas.
///
/// A control that is not shown, or that has no area, holds nothing.
///
/// @param control the control
/// @param x the point's column
/// @param y the point's row
/// @return true when the rectangle holds the point
bool holds_point(const AutomationControl& control, int32_t x, int32_t y) noexcept {
    if (!control.visible || control.width <= 0 || control.height <= 0)
        return false;
    if (x < control.x || y < control.y)
        return false;
    const int64_t right = static_cast<int64_t>(control.x) + static_cast<int64_t>(control.width);
    const int64_t bottom = static_cast<int64_t>(control.y) + static_cast<int64_t>(control.height);
    return static_cast<int64_t>(x) < right && static_cast<int64_t>(y) < bottom;
}

/// Copies a built node and its children out of the build list.
///
/// @param nodes the build list
/// @param index the node's index
/// @return the node
ControlNode materialize(const std::vector<BuildNode>& nodes, std::size_t index) {
    ControlNode node;
    node.group = nodes[index].group;
    node.parent = nodes[index].parent;
    node.control = nodes[index].control;
    node.hovered = nodes[index].hovered;
    const std::vector<std::size_t> children = nodes[index].children;
    node.children.reserve(children.size());
    for (const std::size_t child : children)
        node.children.push_back(materialize(nodes, child));
    return node;
}

} // namespace

ControlParent control_parent(const AutomationControl& control, std::string_view package) {
    ControlParent parent;
    if (!control.window.empty()) {
        parent.kind = control.package_window ? ControlParentKind::window
                                             : ControlParentKind::extension_window;
        parent.name = control.window;
        return parent;
    }
    if (!control.dialog.empty()) {
        parent.kind = ControlParentKind::dialog;
        parent.name = control.dialog;
        return parent;
    }
    parent.kind = ControlParentKind::screen_package;
    parent.name = std::string(package);
    return parent;
}

ControlTree control_tree(
    const std::vector<AutomationControl>& controls,
    std::string_view package,
    std::string_view hovered
) {
    std::vector<BuildNode> nodes;
    std::vector<std::size_t> roots;
    std::vector<std::pair<GroupKey, std::size_t>> groups;
    std::size_t package_index = static_cast<std::size_t>(-1);

    const auto add = [&nodes](BuildNode node) {
        nodes.push_back(std::move(node));
        return nodes.size() - 1;
    };
    const auto find_group = [&groups](const GroupKey& key) {
        for (const auto& [group, index] : groups)
            if (same_group(group, key))
                return index;
        return static_cast<std::size_t>(-1);
    };
    const auto package_group = [&]() {
        if (package_index != static_cast<std::size_t>(-1))
            return package_index;
        BuildNode node;
        node.group = true;
        node.parent.kind = ControlParentKind::screen_package;
        node.parent.name = std::string(package);
        package_index = add(std::move(node));
        roots.push_back(package_index);
        groups.push_back(
            {GroupKey{ControlParentKind::screen_package, std::string(package)}, package_index}
        );
        return package_index;
    };

    for (const AutomationControl& control : controls) {
        const ControlParent parent = control_parent(control, package);
        const GroupKey key{parent.kind, parent.name};
        std::size_t group = find_group(key);
        if (group == static_cast<std::size_t>(-1)) {
            BuildNode node;
            node.group = true;
            node.parent = parent;
            group = add(std::move(node));
            groups.push_back({key, group});
            if (parent.kind == ControlParentKind::extension_window) {
                roots.push_back(group);
            } else if (parent.kind == ControlParentKind::screen_package) {
                package_index = group;
                roots.push_back(group);
            } else {
                nodes[package_group()].children.push_back(group);
            }
        }
        BuildNode leaf;
        leaf.parent = parent;
        leaf.control = control;
        leaf.hovered = !hovered.empty() && control.name == hovered;
        const std::size_t leaf_index = add(std::move(leaf));
        nodes[group].children.push_back(leaf_index);
    }
    if (package_index == static_cast<std::size_t>(-1))
        (void)package_group();

    ControlTree tree;
    tree.hovered = std::string(hovered);
    tree.roots.reserve(roots.size());
    for (const std::size_t root : roots)
        tree.roots.push_back(materialize(nodes, root));
    return tree;
}

ControlListing control_listing(
    const std::vector<AutomationControl>& controls,
    std::string_view package,
    std::string_view hovered
) {
    ControlListing listing;
    listing.flat.reserve(controls.size());
    for (const AutomationControl& control : controls) {
        ListedControl listed;
        listed.control = control;
        listed.parent = control_parent(control, package);
        listed.hovered = !hovered.empty() && control.name == hovered;
        listing.flat.push_back(std::move(listed));
    }
    listing.tree = control_tree(controls, package, hovered);
    return listing;
}

std::string hovered_control(
    const std::vector<AutomationControl>& controls,
    int32_t x,
    int32_t y,
    std::string_view game_hover
) {
    for (const AutomationControl& control : controls)
        if (holds_point(control, x, y))
            return control.name;
    return std::string(game_hover);
}

} // namespace oa::app::automation
