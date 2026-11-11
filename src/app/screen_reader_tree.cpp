// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The screen-reader tree of one control listing (screen_reader_tree.hpp).
#include "oa/app/screen_reader_tree.hpp"

#if !defined(OA_SCREEN_READER_COMPILED_OUT)

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace oa::app::automation {

/// Where the game's window shows the canvas. This is WindowPlacement in
/// input.hpp. That header pulls in the endpoint, which this file does not.
struct WindowPlacement {
    SDL_Window* window{};
    SDL_Renderer* renderer{};
    SDL_WindowID window_id{};
    float density{1.0F};
};

/// Places a canvas point in the window. Defined with the automation input.
///
/// @param placement where the window shows the canvas
/// @param x canvas column
/// @param y canvas row
/// @param[out] window_x the window's column
/// @param[out] window_y the window's row
/// @return false when the renderer cannot place it
[[nodiscard]] bool canvas_to_window(
    const WindowPlacement& placement, float x, float y, float& window_x, float& window_y
);

} // namespace oa::app::automation

namespace {

namespace bridge = oa::platform::screen_reader;
namespace automation = oa::app::automation;

constexpr uint64_t kIdOffset = 14695981039346656037ULL;
constexpr uint64_t kIdPrime = 1099511628211ULL;
constexpr std::size_t kNoNode = static_cast<std::size_t>(-1);
constexpr double kInside = 0.5;
constexpr const char* kMenuOverlay = "MainMenuOverlay";
constexpr const char* kMainMenu = "main_menu";

/// Folds a string into an id.
///
/// @param hash the id so far
/// @param text the string
/// @return the id
uint64_t mix_text(uint64_t hash, std::string_view text) {
    for (const char byte : text) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= kIdPrime;
    }
    hash ^= 0x1fU;
    hash *= kIdPrime;
    return hash;
}

/// An id for one control. 0 and the window id are moved off, and a clash
/// takes the next free id.
///
/// @param name the control's name
/// @param dialog the dialog it belongs to
/// @param window the window it belongs to
/// @param ordinal which copy, when the same name is listed twice
/// @param[in,out] used the ids already given
/// @return the id
uint64_t claim_id(
    std::string_view name,
    std::string_view dialog,
    std::string_view window,
    uint32_t ordinal,
    std::unordered_set<uint64_t>& used
) {
    uint64_t hash = mix_text(kIdOffset, name);
    hash = mix_text(hash, dialog);
    hash = mix_text(hash, window);
    for (int shift = 0; shift < 32; shift += 8) {
        hash ^= static_cast<unsigned char>((ordinal >> shift) & 0xffU);
        hash *= kIdPrime;
    }
    if (hash == 0 || hash == bridge::window_node)
        hash += 2;
    while (hash == 0 || hash == bridge::window_node || !used.insert(hash).second) {
        ++hash;
        if (hash == 0)
            hash = 2;
    }
    return hash;
}

/// The next copy number for a name in one dialog and window.
///
/// @param name the control's name
/// @param dialog the dialog
/// @param window the window
/// @param[in,out] seen how many times each key has been used
/// @return the copy number, from 0
uint32_t next_ordinal(
    std::string_view name,
    std::string_view dialog,
    std::string_view window,
    std::unordered_map<std::string, uint32_t>& seen
) {
    std::string key;
    key.reserve(name.size() + dialog.size() + window.size() + 2);
    key.append(name);
    key.push_back('\n');
    key.append(dialog);
    key.push_back('\n');
    key.append(window);
    const uint32_t ordinal = seen[key];
    ++seen[key];
    return ordinal;
}

/// Reports whether a control is left out of the tree.
///
/// @param control the control
/// @return true when it is hidden, not shown, or decoration
bool left_out(const oa::app::AutomationControl& control) {
    return control.hidden || !control.visible ||
           control.role == oa::app::AutomationControlRole::none;
}

/// Reports whether two names are the same, ignoring ASCII case.
///
/// @param left one name
/// @param right the other
/// @return true when they match
bool same_name(std::string_view left, std::string_view right) {
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

/// The screen-reader role of an automation role.
///
/// @param role the automation role
/// @return the bridge role
bridge::Role role_of(oa::app::AutomationControlRole role) {
    switch (role) {
    case oa::app::AutomationControlRole::button:
        return bridge::Role::button;
    case oa::app::AutomationControlRole::check_box:
        return bridge::Role::check_box;
    case oa::app::AutomationControlRole::switch_control:
        return bridge::Role::switch_control;
    case oa::app::AutomationControlRole::radio_group:
        return bridge::Role::radio_group;
    case oa::app::AutomationControlRole::radio:
        return bridge::Role::radio;
    case oa::app::AutomationControlRole::tab_list:
        return bridge::Role::tab_list;
    case oa::app::AutomationControlRole::tab:
        return bridge::Role::tab;
    case oa::app::AutomationControlRole::list:
        return bridge::Role::list;
    case oa::app::AutomationControlRole::list_item:
        return bridge::Role::list_item;
    case oa::app::AutomationControlRole::tree:
        return bridge::Role::tree;
    case oa::app::AutomationControlRole::tree_item:
        return bridge::Role::tree_item;
    case oa::app::AutomationControlRole::text_field:
        return bridge::Role::text_field;
    case oa::app::AutomationControlRole::password:
        return bridge::Role::password;
    case oa::app::AutomationControlRole::search:
        return bridge::Role::search;
    case oa::app::AutomationControlRole::slider:
        return bridge::Role::slider;
    case oa::app::AutomationControlRole::log:
        return bridge::Role::log;
    case oa::app::AutomationControlRole::dialog:
        return bridge::Role::dialog;
    case oa::app::AutomationControlRole::alert_dialog:
        return bridge::Role::alert_dialog;
    case oa::app::AutomationControlRole::group:
        return bridge::Role::group;
    case oa::app::AutomationControlRole::image:
        return bridge::Role::image;
    case oa::app::AutomationControlRole::link:
        return bridge::Role::link;
    case oa::app::AutomationControlRole::label:
        return bridge::Role::label;
    case oa::app::AutomationControlRole::none:
        return bridge::Role::none;
    }
    return bridge::Role::none;
}

/// How a change is announced.
///
/// @param live the automation setting
/// @return the bridge setting
bridge::LiveSetting live_of(oa::app::AutomationLive live) {
    switch (live) {
    case oa::app::AutomationLive::polite:
        return bridge::LiveSetting::polite;
    case oa::app::AutomationLive::assertive:
        return bridge::LiveSetting::assertive;
    case oa::app::AutomationLive::off:
        return bridge::LiveSetting::off;
    }
    return bridge::LiveSetting::off;
}

/// The window's pixels to one point.
///
/// @param placement where the window shows the canvas
/// @return the density; 1 when it is not known
double density_of(const automation::WindowPlacement& placement) {
    return placement.density > 0.0F ? static_cast<double>(placement.density) : 1.0;
}

/// Writes a control's bounds in window points.
///
/// @param[in,out] node the node that gains the bounds
/// @param control the control
/// @param placement where the window shows the canvas
void put_bounds(
    bridge::Node& node,
    const oa::app::AutomationControl& control,
    const automation::WindowPlacement& placement
) {
    if (control.window_pixels) {
        const double density = density_of(placement);
        node.x = static_cast<double>(control.window_x) / density;
        node.y = static_cast<double>(control.window_y) / density;
        node.width = static_cast<double>(control.window_width) / density;
        node.height = static_cast<double>(control.window_height) / density;
        return;
    }
    float left = 0.0F;
    float top = 0.0F;
    float right = 0.0F;
    float bottom = 0.0F;
    if (!automation::canvas_to_window(
            placement, static_cast<float>(control.x), static_cast<float>(control.y), left, top
        ) ||
        !automation::canvas_to_window(
            placement,
            static_cast<float>(control.x + control.width),
            static_cast<float>(control.y + control.height),
            right,
            bottom
        ))
        return;
    node.x = left;
    node.y = top;
    node.width = static_cast<double>(right) - left;
    node.height = static_cast<double>(bottom) - top;
}

/// Reports whether a rectangle lies inside another, with a little slack for
/// the points a shared edge lands on.
///
/// @param outer the container
/// @param inner the row
/// @return true when the row lies inside the container
bool contains(const bridge::Node& outer, const bridge::Node& inner) {
    if (outer.width <= 0.0 || outer.height <= 0.0 || inner.width <= 0.0 || inner.height <= 0.0)
        return false;
    return inner.x >= outer.x - kInside && inner.y >= outer.y - kInside &&
           inner.x + inner.width <= outer.x + outer.width + kInside &&
           inner.y + inner.height <= outer.y + outer.height + kInside;
}

/// The row a container adopts.
///
/// @param role the container's role
/// @return the row's role, or none when the role is not such a container
bridge::Role row_of(bridge::Role role) {
    switch (role) {
    case bridge::Role::tab_list:
        return bridge::Role::tab;
    case bridge::Role::list:
        return bridge::Role::list_item;
    case bridge::Role::tree:
        return bridge::Role::tree_item;
    case bridge::Role::radio_group:
        return bridge::Role::radio;
    default:
        return bridge::Role::none;
    }
}

/// One list, tree or log whose rows may be built from its items.
struct RowSource {
    std::size_t index{};
    oa::app::AutomationControl control{};
};

/// The open group a following row belongs to.
struct OpenGroups {
    std::size_t radio{kNoNode};
    std::size_t tabs{kNoNode};
    std::size_t list{kNoNode};
    std::size_t tree{kNoNode};
};

/// A focused control the tree left out, so a visible button can stand for it.
struct CoveredFocus {
    bool set{};
    std::string name;
    std::string label;
};

/// Builds the bridge tree from one listing.
class TreeBuilder {
  public:

    /// Starts from the window.
    ///
    /// @param placement where the window shows the canvas
    /// @param keep_menu_overlay false when a screen other than the main menu is showing
    TreeBuilder(const automation::WindowPlacement& placement, bool keep_menu_overlay)
        : placement_(placement), keep_menu_overlay_(keep_menu_overlay) {
        bridge::Node root;
        root.id = bridge::window_node;
        root.role = bridge::Role::window;
        root.label = "Open Annihilation";
        if (placement.window != nullptr) {
            int width = 0;
            int height = 0;
            SDL_GetWindowSize(placement.window, &width, &height);
            root.width = static_cast<double>(width);
            root.height = static_cast<double>(height);
        }
        used_.insert(bridge::window_node);
        push(std::move(root));
    }

    /// Adds one root of the control tree under the window.
    ///
    /// @param root an extension window, or the screen package
    void add_root(const automation::ControlNode& root) {
        if (root.group && root.parent.kind == automation::ControlParentKind::extension_window &&
            root.parent.name == kMenuOverlay && !keep_menu_overlay_)
            return;
        if (!root.group) {
            add_leaf_to(0, root.control, false);
            return;
        }
        if (root.parent.kind == automation::ControlParentKind::extension_window)
            add_group(0, root);
        else
            map_children(0, root.children, false);
    }

    /// The nodes, with any control a parent does not reach left out, and the
    /// id the reader is told holds the focus.
    ///
    /// @param[out] focus the focused id
    /// @return the nodes
    std::vector<bridge::Node> finish(uint64_t& focus) {
        settle_scroll_bars();
        group_loose_tabs();
        unlink_empty();
        size_containers();
        fit_groups();
        order_children();
        hand_covered_focus();
        focus = focused_id();
        return reachable();
    }

  private:

    /// Appends a node.
    ///
    /// @param node the node
    /// @return its index
    std::size_t push(bridge::Node node) {
        const uint64_t id = node.id;
        const std::size_t index = nodes_.size();
        id_at_.emplace(id, index);
        nodes_.push_back(std::move(node));
        return index;
    }

    /// The node of an id.
    ///
    /// @param id the id
    /// @return the node, or null when the id is not in the tree
    bridge::Node* at(uint64_t id) {
        const auto found = id_at_.find(id);
        if (found == id_at_.end())
            return nullptr;
        return &nodes_[found->second];
    }

    /// The node of an id.
    ///
    /// @param id the id
    /// @return the node, or null when the id is not in the tree
    const bridge::Node* at(uint64_t id) const {
        const auto found = id_at_.find(id);
        if (found == id_at_.end())
            return nullptr;
        return &nodes_[found->second];
    }

    /// Remembers a focused button the tree is about to leave out.
    ///
    /// @param control the button
    void note_covered(const oa::app::AutomationControl& control) {
        if (control.role != oa::app::AutomationControlRole::button || !control.focused)
            return;
        covered_.set = true;
        covered_.name = control.name;
        covered_.label = control.label;
    }

    /// Maps a control onto a node. A masked field's value is one masking
    /// character for each character it holds. A field that is not masked
    /// gives its text as its value. An empty label is left out.
    ///
    /// @param control the control
    /// @param demote_alert true when an alert's text is a label under the dialog
    /// @return the node; its id is 0 when the control is left out
    bridge::Node node_from(const oa::app::AutomationControl& control, bool demote_alert) const {
        bridge::Node node;
        if (left_out(control))
            return node;
        node.role = role_of(control.role);
        if (demote_alert && node.role == bridge::Role::alert_dialog)
            node.role = bridge::Role::label;
        node.label = control.label;
        if (node.role == bridge::Role::password) {
            // U+2022, the character AccessKit masks with. Digits would be
            // read as the text, or masked as two or three characters.
            for (int32_t held = 0; held < control.text_length; ++held)
                node.value_text += "\xE2\x80\xA2";
        } else {
            node.value_text = control.value_text;
            const bool typed =
                node.role == bridge::Role::text_field || node.role == bridge::Role::search;
            if (typed && node.value_text.empty() && !control.masked)
                node.value_text = control.text;
        }
        if (node.role == bridge::Role::label && node.label.empty() && node.value_text.empty())
            return bridge::Node{};
        // A picture with no name, the GUI's `text` placeholder, and a field
        // with no name and no label are not controls a reader can use.
        if (node.role == bridge::Role::image && node.label.empty())
            return bridge::Node{};
        if (node.role == bridge::Role::label && node.label == "text")
            return bridge::Node{};
        if (node.role == bridge::Role::text_field && control.name.empty() && node.label.empty())
            return bridge::Node{};
        node.description = control.description;
        node.shortcut = control.shortcut;
        node.has_value = control.has_value;
        node.value = static_cast<double>(control.value);
        node.has_range = control.has_range;
        node.least = static_cast<double>(control.range_least);
        node.most = static_cast<double>(control.range_most);
        node.step = static_cast<double>(control.step);
        if (node.role == bridge::Role::check_box || node.role == bridge::Role::switch_control) {
            node.toggled_known = true;
            node.toggled = control.checked || control.pressed;
        } else if (node.role == bridge::Role::button && control.pressed) {
            node.toggled_known = true;
            node.toggled = true;
        }
        const bool chooses = node.role == bridge::Role::radio || node.role == bridge::Role::tab ||
                             node.role == bridge::Role::list_item ||
                             node.role == bridge::Role::tree_item;
        if (chooses || control.chosen) {
            node.selected_known = true;
            node.selected = control.chosen;
        }
        if (node.role == bridge::Role::tree || node.role == bridge::Role::tree_item ||
            control.expanded) {
            node.expanded_known = true;
            node.expanded = control.expanded;
        }
        node.disabled = !control.enabled;
        node.focused = control.focused;
        node.steps = control.steps;
        node.tab_index = control.tab_index;
        node.place = control.place;
        node.set_size = control.set_size;
        node.live = live_of(control.live);
        if (node.role == bridge::Role::log && node.live == bridge::LiveSetting::off)
            node.live = bridge::LiveSetting::polite;
        put_bounds(node, control, placement_);
        node.id = claim_id(
            control.name,
            control.dialog,
            control.window,
            next_ordinal(control.name, control.dialog, control.window, seen_),
            used_
        );
        return node;
    }

    /// Reports whether a check box is one half of a switch already added
    /// under the same parent.
    ///
    /// @param parent the parent
    /// @param box the check box
    /// @return true when a switch's rectangle holds the box
    bool switch_segment(std::size_t parent, const bridge::Node& box) const {
        if (box.role != bridge::Role::check_box || parent >= nodes_.size())
            return false;
        for (const uint64_t child : nodes_[parent].children) {
            const bridge::Node* host = at(child);
            if (host != nullptr && host->role == bridge::Role::switch_control &&
                contains(*host, box))
                return true;
        }
        return false;
    }

    /// Adds a leaf under the group that should hold it.
    ///
    /// @param parent the group it belongs to when it is not a row of an open group
    /// @param control the control
    /// @param demote_alert true inside an alert dialog
    /// @param[in,out] open the groups a following row can join
    /// @param[in,out] sources lists whose rows may be built from their items
    void add_leaf_to(
        std::size_t parent,
        const oa::app::AutomationControl& control,
        bool demote_alert,
        OpenGroups* open,
        std::vector<RowSource>* sources
    ) {
        if (left_out(control)) {
            note_covered(control);
            return;
        }
        bridge::Node node = node_from(control, demote_alert);
        if (node.id == 0)
            return;
        if (switch_segment(parent, node))
            return;
        const bridge::Role role = node.role;
        const std::size_t index = push(std::move(node));
        const std::size_t host = host_of(open, role, parent);
        nodes_[host].children.push_back(nodes_[index].id);
        if (open != nullptr)
            remember_open(*open, index, role);
        if (sources != nullptr &&
            (role == bridge::Role::list || role == bridge::Role::tree || role == bridge::Role::log))
            sources->push_back(RowSource{index, control});
    }

    /// Adds a leaf that is a root by itself.
    ///
    /// @param parent the window
    /// @param control the control
    /// @param demote_alert unused at a root
    void
    add_leaf_to(std::size_t parent, const oa::app::AutomationControl& control, bool demote_alert) {
        add_leaf_to(parent, control, demote_alert, nullptr, nullptr);
    }

    /// The group a row joins, or the parent it was listed under.
    ///
    /// @param open the open groups; null when there are none
    /// @param role the row's role
    /// @param parent the parent it was listed under
    /// @return the index to attach it to
    std::size_t host_of(const OpenGroups* open, bridge::Role role, std::size_t parent) const {
        if (open == nullptr)
            return parent;
        if (role == bridge::Role::radio && open->radio != kNoNode)
            return open->radio;
        if (role == bridge::Role::tab && open->tabs != kNoNode)
            return open->tabs;
        if (role == bridge::Role::list_item && open->list != kNoNode)
            return open->list;
        if (role == bridge::Role::tree_item && open->tree != kNoNode)
            return open->tree;
        return parent;
    }

    /// Remembers a group the following rows can join.
    ///
    /// @param[in,out] open the open groups
    /// @param index the group
    /// @param role its role
    void remember_open(OpenGroups& open, std::size_t index, bridge::Role role) const {
        if (role == bridge::Role::radio_group)
            open.radio = index;
        else if (role == bridge::Role::tab_list)
            open.tabs = index;
        else if (role == bridge::Role::list)
            open.list = index;
        else if (role == bridge::Role::tree)
            open.tree = index;
    }

    /// A container adopts the rows of its kind listed before it whose
    /// rectangles lie inside its own. A row joins the first such container.
    ///
    /// @param parent the dialog or window the rows were listed under
    void adopt_earlier_rows(std::size_t parent) {
        if (parent >= nodes_.size())
            return;
        const std::vector<uint64_t> kids = nodes_[parent].children;
        std::vector<char> taken(kids.size(), 0);
        for (std::size_t place = 0; place < kids.size(); ++place) {
            if (taken[place] != 0)
                continue;
            bridge::Node* container = at(kids[place]);
            if (container == nullptr)
                continue;
            const bridge::Role wanted = row_of(container->role);
            if (wanted == bridge::Role::none)
                continue;
            std::vector<uint64_t> adopted;
            for (std::size_t earlier = 0; earlier < place; ++earlier) {
                if (taken[earlier] != 0)
                    continue;
                bridge::Node* row = at(kids[earlier]);
                if (row == nullptr || row->role != wanted || !contains(*container, *row))
                    continue;
                adopted.push_back(kids[earlier]);
                taken[earlier] = 1;
            }
            if (!adopted.empty())
                container->children.insert(
                    container->children.begin(), adopted.begin(), adopted.end()
                );
        }
        bool any = false;
        for (const char bit : taken)
            any = any || bit != 0;
        if (!any)
            return;
        std::vector<uint64_t> kept;
        kept.reserve(kids.size());
        for (std::size_t index = 0; index < kids.size(); ++index) {
            if (taken[index] == 0)
                kept.push_back(kids[index]);
        }
        nodes_[parent].children = std::move(kept);
    }

    /// Maps a group's children, nesting rows under the group they follow,
    /// then under a container listed after them.
    ///
    /// @param parent the node the children attach to
    /// @param children the group's children
    /// @param demote_alert true inside an alert dialog
    void map_children(
        std::size_t parent, const std::vector<automation::ControlNode>& children, bool demote_alert
    ) {
        OpenGroups open;
        std::vector<RowSource> sources;
        for (const automation::ControlNode& child : children) {
            if (child.group) {
                if (child.parent.kind == automation::ControlParentKind::dialog)
                    add_dialog(parent, child);
                else if (
                    child.parent.kind == automation::ControlParentKind::window ||
                    child.parent.kind == automation::ControlParentKind::extension_window
                )
                    add_group(parent, child);
                else
                    map_children(parent, child.children, demote_alert);
                continue;
            }
            add_leaf_to(parent, child.control, demote_alert, &open, &sources);
        }
        adopt_earlier_rows(parent);
        for (const RowSource& source : sources)
            synthesize_rows(source);
        adopt_contained_buttons(parent, sources);
    }

    /// Adds a dialog, named from its alert or its title, never from a label.
    ///
    /// @param parent the node the dialog attaches to
    /// @param group the dialog's group
    void add_dialog(std::size_t parent, const automation::ControlNode& group) {
        const bool alert = holds_alert(group);
        bridge::Node dialog;
        dialog.role = alert ? bridge::Role::alert_dialog : bridge::Role::dialog;
        dialog.label = dialog_name(group, alert);
        dialog.modal = holds_modal(group);
        dialog.id = claim_id(
            group.parent.name,
            "dialog",
            "",
            next_ordinal(group.parent.name, "dialog", "", seen_),
            used_
        );
        const std::size_t index = push(std::move(dialog));
        map_children(index, group.children, alert);
        if (!nodes_[index].children.empty())
            nodes_[parent].children.push_back(nodes_[index].id);
    }

    /// Adds a group for a console or a window. Its name is not the window's
    /// internal name.
    ///
    /// @param parent the node the group attaches to
    /// @param group the window's group
    void add_group(std::size_t parent, const automation::ControlNode& group) {
        bridge::Node node;
        node.role = bridge::Role::group;
        node.id = claim_id(
            group.parent.name,
            "window",
            group.parent.name,
            next_ordinal(group.parent.name, "window", group.parent.name, seen_),
            used_
        );
        const std::size_t index = push(std::move(node));
        map_children(index, group.children, false);
        if (!nodes_[index].children.empty())
            nodes_[parent].children.push_back(nodes_[index].id);
    }

    /// Reports whether a group holds an alert's text.
    ///
    /// @param group the group
    /// @return true when a control in it is an alert
    bool holds_alert(const automation::ControlNode& group) const {
        if (!group.group) {
            return !left_out(group.control) &&
                   group.control.role == oa::app::AutomationControlRole::alert_dialog;
        }
        for (const automation::ControlNode& child : group.children) {
            if (holds_alert(child))
                return true;
        }
        return false;
    }

    /// Reports whether a dialog covers the screen.
    ///
    /// @param group the group
    /// @return true when a control in it is modal
    bool holds_modal(const automation::ControlNode& group) const {
        if (!group.group)
            return !left_out(group.control) && group.control.modal;
        for (const automation::ControlNode& child : group.children) {
            if (holds_modal(child))
                return true;
        }
        return false;
    }

    /// The name of a dialog: the alert's words, else a title the screen lists,
    /// else none. The first label is not the name.
    ///
    /// @param group the dialog
    /// @param alert true when the dialog is an alert
    /// @return the name
    std::string dialog_name(const automation::ControlNode& group, bool alert) const {
        std::string alert_label;
        std::string title;
        collect_names(group, alert_label, title);
        if (alert && !alert_label.empty())
            return alert_label;
        return title;
    }

    /// Collects the alert's words and a listed title under a group.
    ///
    /// @param group the group
    /// @param[in,out] alert_label the alert's words, left as they are once set
    /// @param[in,out] title the dialog's title, left as it is once set
    void collect_names(
        const automation::ControlNode& group, std::string& alert_label, std::string& title
    ) const {
        if (!group.group) {
            if (left_out(group.control))
                return;
            if (alert_label.empty() &&
                group.control.role == oa::app::AutomationControlRole::alert_dialog &&
                !group.control.label.empty())
                alert_label = group.control.label;
            if (title.empty() && !group.control.title.empty())
                title = group.control.title;
            if (title.empty() && same_name(group.control.name, "TITLE")) {
                if (!group.control.label.empty())
                    title = group.control.label;
                else if (!group.control.text.empty())
                    title = group.control.text;
            }
            return;
        }
        for (const automation::ControlNode& child : group.children)
            collect_names(child, alert_label, title);
    }

    /// Builds rows from a list's items when the tree did not list them.
    ///
    /// @param source the list, tree or log
    void synthesize_rows(const RowSource& source) {
        if (source.index >= nodes_.size() || source.control.items.empty())
            return;
        const bridge::Role parent_role = nodes_[source.index].role;
        bridge::Role row_role = bridge::Role::label;
        if (parent_role == bridge::Role::list)
            row_role = bridge::Role::list_item;
        else if (parent_role == bridge::Role::tree)
            row_role = bridge::Role::tree_item;
        else if (parent_role != bridge::Role::log)
            return;
        if (has_child_role(source.index, row_role))
            return;
        const double origin_x = nodes_[source.index].x;
        const double origin_y = nodes_[source.index].y;
        const double origin_width = nodes_[source.index].width;
        const double origin_height = nodes_[source.index].height;
        const bool row_disabled = nodes_[source.index].disabled;
        const int32_t count = static_cast<int32_t>(source.control.items.size());
        for (int32_t row = 0; row < count; ++row) {
            bridge::Node item;
            item.role = row_role;
            item.label = source.control.items[static_cast<std::size_t>(row)];
            item.place = row + 1;
            item.set_size = count;
            if (row_role != bridge::Role::label) {
                item.selected_known = true;
                item.selected = source.control.selected == row;
            }
            item.disabled = row_disabled;
            item.x = origin_x;
            item.y = origin_y;
            item.width = origin_width;
            item.height = origin_height;
            if (source.control.rows > 0 && row >= source.control.first_visible &&
                row < source.control.first_visible + source.control.rows) {
                const double row_height = origin_height / static_cast<double>(source.control.rows);
                item.y =
                    origin_y + static_cast<double>(row - source.control.first_visible) * row_height;
                item.height = row_height;
            }
            item.id = claim_id(
                item.label,
                source.control.dialog,
                source.control.window,
                next_ordinal(item.label, source.control.dialog, source.control.window, seen_),
                used_
            );
            const std::size_t index = push(std::move(item));
            nodes_[source.index].children.push_back(nodes_[index].id);
        }
    }

    /// Reports whether a node already has a child of one role.
    ///
    /// @param index the node
    /// @param role the role
    /// @return true when it has one
    bool has_child_role(std::size_t index, bridge::Role role) const {
        if (index >= nodes_.size())
            return false;
        for (const uint64_t child : nodes_[index].children) {
            const bridge::Node* node = at(child);
            if (node != nullptr && node->role == role)
                return true;
        }
        return false;
    }

    /// Names a scroll bar after its list, or leaves it out when it has no
    /// label and no list.
    void settle_scroll_bars() {
        for (std::size_t parent = 0; parent < nodes_.size(); ++parent) {
            std::string list_name;
            if ((nodes_[parent].role == bridge::Role::list ||
                 nodes_[parent].role == bridge::Role::tree) &&
                !nodes_[parent].label.empty())
                list_name = nodes_[parent].label;
            for (const uint64_t child : nodes_[parent].children) {
                const bridge::Node* node = at(child);
                if (node == nullptr)
                    continue;
                if ((node->role == bridge::Role::list || node->role == bridge::Role::tree) &&
                    !node->label.empty()) {
                    list_name = node->label;
                    break;
                }
            }
            std::vector<uint64_t> kept;
            kept.reserve(nodes_[parent].children.size());
            for (const uint64_t child : nodes_[parent].children) {
                bridge::Node* node = at(child);
                if (node == nullptr)
                    continue;
                if (node->role == bridge::Role::slider && node->label.empty()) {
                    if (list_name.empty())
                        continue;
                    node->label = list_name;
                }
                kept.push_back(child);
            }
            nodes_[parent].children = std::move(kept);
        }
    }

    /// Gives a tab list, list, tree or radio group the size of the set its
    /// rows carry, when it has none of its own. AccessKit reads a row's set
    /// size from its container, not from the row.
    void size_containers() {
        for (bridge::Node& node : nodes_) {
            if (row_of(node.role) == bridge::Role::none || node.set_size > 0)
                continue;
            for (const uint64_t child : node.children) {
                const bridge::Node* row = at(child);
                if (row != nullptr && row->set_size > 0) {
                    node.set_size = row->set_size;
                    break;
                }
            }
        }
    }

    /// Drops a dialog, a group or a list that has no children. A list that
    /// holds the game's focus stays, empty, so the reader is told where the
    /// focus is: Load Game with no saved games focuses its empty list.
    void unlink_empty() {
        for (bridge::Node& parent : nodes_) {
            std::vector<uint64_t> kept;
            kept.reserve(parent.children.size());
            for (const uint64_t child : parent.children) {
                const bridge::Node* node = at(child);
                if (node == nullptr)
                    continue;
                const bool container = node->role == bridge::Role::dialog ||
                                       node->role == bridge::Role::alert_dialog ||
                                       node->role == bridge::Role::group ||
                                       node->role == bridge::Role::list;
                if (container && node->children.empty() && !node->focused)
                    continue;
                kept.push_back(child);
            }
            parent.children = std::move(kept);
        }
    }

    /// Gives one group or dialog the union of its children's bounds.
    ///
    /// @param index the group or dialog
    void fit_one(std::size_t index) {
        bool any = false;
        double left = 0.0;
        double top = 0.0;
        double right = 0.0;
        double bottom = 0.0;
        for (const uint64_t child : nodes_[index].children) {
            const bridge::Node* node = at(child);
            if (node == nullptr || node->width <= 0.0 || node->height <= 0.0)
                continue;
            const double edge_x = node->x + node->width;
            const double edge_y = node->y + node->height;
            if (!any) {
                left = node->x;
                top = node->y;
                right = edge_x;
                bottom = edge_y;
                any = true;
                continue;
            }
            left = std::min(left, node->x);
            top = std::min(top, node->y);
            right = std::max(right, edge_x);
            bottom = std::max(bottom, edge_y);
        }
        if (!any)
            return;
        nodes_[index].x = left;
        nodes_[index].y = top;
        nodes_[index].width = right - left;
        nodes_[index].height = bottom - top;
    }

    /// Gives every group and dialog the union of its children's bounds.
    /// Children are fitted before the group that holds them.
    void fit_groups() {
        for (std::size_t index = nodes_.size(); index-- > 0;) {
            const bridge::Role role = nodes_[index].role;
            if (role == bridge::Role::dialog || role == bridge::Role::alert_dialog ||
                role == bridge::Role::group)
                fit_one(index);
        }
    }

    /// A list with no rows of its own adopts the buttons inside its rectangle.
    /// A button joins the earliest such list. A list that will grow rows from
    /// its items does not adopt buttons.
    ///
    /// @param parent the dialog or window the list was listed under
    /// @param sources the lists, trees and logs under that parent
    void adopt_contained_buttons(std::size_t parent, const std::vector<RowSource>& sources) {
        if (parent >= nodes_.size())
            return;
        std::vector<uint64_t> lists;
        for (const RowSource& source : sources) {
            if (source.index >= nodes_.size())
                continue;
            const bridge::Node& node = nodes_[source.index];
            if (node.role != bridge::Role::list || !node.children.empty() ||
                !source.control.items.empty())
                continue;
            lists.push_back(node.id);
        }
        if (lists.empty())
            return;
        const std::vector<uint64_t> kids = nodes_[parent].children;
        std::vector<char> taken(kids.size(), 0);
        for (std::size_t list_place = 0; list_place < kids.size(); ++list_place) {
            bool listed = false;
            for (const uint64_t list_id : lists) {
                if (kids[list_place] == list_id)
                    listed = true;
            }
            if (!listed)
                continue;
            bridge::Node* list = at(kids[list_place]);
            if (list == nullptr)
                continue;
            for (std::size_t place = 0; place < kids.size(); ++place) {
                if (taken[place] != 0 || place == list_place)
                    continue;
                bridge::Node* button = at(kids[place]);
                if (button == nullptr || button->role != bridge::Role::button)
                    continue;
                if (!contains(*list, *button))
                    continue;
                list->children.push_back(kids[place]);
                taken[place] = 1;
            }
            if (list->set_size <= 0)
                list->set_size = static_cast<int32_t>(list->children.size());
        }
        bool any = false;
        for (const char bit : taken)
            any = any || bit != 0;
        if (!any)
            return;
        std::vector<uint64_t> kept;
        kept.reserve(kids.size());
        for (std::size_t index = 0; index < kids.size(); ++index) {
            if (taken[index] == 0)
                kept.push_back(kids[index]);
        }
        nodes_[parent].children = std::move(kept);
    }

    /// Reports whether two rectangles share a vertical band.
    ///
    /// @param left one control
    /// @param right the other
    /// @return true when their rows overlap
    bool rows_overlap(const bridge::Node& left, const bridge::Node& right) const {
        return left.y < right.y + right.height && right.y < left.y + left.height;
    }

    /// Reports whether two rectangles share a horizontal band.
    ///
    /// @param left one control
    /// @param right the other
    /// @return true when their columns overlap
    bool columns_overlap(const bridge::Node& left, const bridge::Node& right) const {
        return left.x < right.x + right.width && right.x < left.x + left.width;
    }

    /// Groups loose tabs under one parent into tab lists.
    ///
    /// Tabs that share a row become one list. Tabs that share a column, and
    /// were not already in a row, become another. A tab already in a tab list
    /// stays there. One leftover tab stays loose.
    ///
    /// @param parent the dialog or window
    void group_tabs_under(std::size_t parent) {
        if (parent >= nodes_.size())
            return;
        std::vector<uint64_t> tabs;
        for (const uint64_t child : nodes_[parent].children) {
            const bridge::Node* node = at(child);
            if (node != nullptr && node->role == bridge::Role::tab)
                tabs.push_back(child);
        }
        if (tabs.size() < 2)
            return;
        std::vector<std::size_t> link(tabs.size());
        for (std::size_t index = 0; index < tabs.size(); ++index)
            link[index] = index;
        const auto find = [&](std::size_t index) {
            while (link[index] != index)
                index = link[index];
            return index;
        };
        const auto join = [&](std::size_t left, std::size_t right) {
            const bridge::Node* a = at(tabs[left]);
            const bridge::Node* b = at(tabs[right]);
            if (a == nullptr || b == nullptr)
                return;
            link[find(right)] = find(left);
        };
        for (std::size_t left = 0; left < tabs.size(); ++left) {
            const bridge::Node* a = at(tabs[left]);
            if (a == nullptr)
                continue;
            for (std::size_t right = left + 1; right < tabs.size(); ++right) {
                const bridge::Node* b = at(tabs[right]);
                if (b != nullptr && rows_overlap(*a, *b))
                    join(left, right);
            }
        }
        std::vector<std::vector<uint64_t>> rows;
        std::unordered_map<std::size_t, std::size_t> row_of_root;
        for (std::size_t index = 0; index < tabs.size(); ++index) {
            const std::size_t root = find(index);
            const auto found = row_of_root.find(root);
            std::size_t slot = 0;
            if (found == row_of_root.end()) {
                slot = rows.size();
                row_of_root.emplace(root, slot);
                rows.emplace_back();
            } else {
                slot = found->second;
            }
            rows[slot].push_back(tabs[index]);
        }
        std::vector<uint64_t> single;
        for (const std::vector<uint64_t>& row : rows) {
            if (row.size() >= 2)
                make_tab_list(parent, row);
            else if (row.size() == 1)
                single.push_back(row[0]);
        }
        if (single.size() < 2)
            return;
        std::vector<std::size_t> column_link(single.size());
        for (std::size_t index = 0; index < single.size(); ++index)
            column_link[index] = index;
        const auto find_column = [&](std::size_t index) {
            while (column_link[index] != index)
                index = column_link[index];
            return index;
        };
        for (std::size_t left = 0; left < single.size(); ++left) {
            const bridge::Node* a = at(single[left]);
            if (a == nullptr)
                continue;
            for (std::size_t right = left + 1; right < single.size(); ++right) {
                const bridge::Node* b = at(single[right]);
                if (b != nullptr && columns_overlap(*a, *b))
                    column_link[find_column(right)] = find_column(left);
            }
        }
        std::vector<std::vector<uint64_t>> columns;
        std::unordered_map<std::size_t, std::size_t> column_of;
        for (std::size_t index = 0; index < single.size(); ++index) {
            const std::size_t root = find_column(index);
            const auto found = column_of.find(root);
            if (found == column_of.end()) {
                column_of.emplace(root, columns.size());
                columns.push_back({single[index]});
            } else {
                columns[found->second].push_back(single[index]);
            }
        }
        for (const std::vector<uint64_t>& column : columns) {
            if (column.size() >= 2)
                make_tab_list(parent, column);
        }
    }

    /// Makes one tab list from tabs and puts it where the first of them was.
    /// A tab with no place or size of its own takes them from this list.
    ///
    /// @param parent the dialog or window
    /// @param tabs the tabs, in the order they were listed
    void make_tab_list(std::size_t parent, const std::vector<uint64_t>& tabs) {
        if (tabs.size() < 2 || parent >= nodes_.size())
            return;
        bridge::Node list;
        list.role = bridge::Role::tab_list;
        list.set_size = static_cast<int32_t>(tabs.size());
        bool any = false;
        double left = 0.0;
        double top = 0.0;
        double right = 0.0;
        double bottom = 0.0;
        std::string key = "tabs";
        for (const uint64_t id : tabs) {
            const bridge::Node* tab = at(id);
            if (tab == nullptr)
                continue;
            key.push_back('\n');
            key += tab->label;
            if (tab->width <= 0.0 || tab->height <= 0.0)
                continue;
            const double edge_x = tab->x + tab->width;
            const double edge_y = tab->y + tab->height;
            if (!any) {
                left = tab->x;
                top = tab->y;
                right = edge_x;
                bottom = edge_y;
                any = true;
                continue;
            }
            left = std::min(left, tab->x);
            top = std::min(top, tab->y);
            right = std::max(right, edge_x);
            bottom = std::max(bottom, edge_y);
        }
        if (any) {
            list.x = left;
            list.y = top;
            list.width = right - left;
            list.height = bottom - top;
        }
        list.id = claim_id(key, "tab-list", "", next_ordinal(key, "tab-list", "", seen_), used_);
        const std::size_t index = push(std::move(list));
        const int32_t count = static_cast<int32_t>(tabs.size());
        int32_t place = 0;
        for (const uint64_t id : tabs) {
            nodes_[index].children.push_back(id);
            ++place;
            bridge::Node* tab = at(id);
            if (tab == nullptr)
                continue;
            // A tab the screen gave no place takes its place in this list,
            // so a reader hears "2 of 18" and not the size alone.
            if (tab->set_size <= 0)
                tab->set_size = count;
            if (tab->place <= 0)
                tab->place = place;
        }
        std::vector<uint64_t> kept;
        bool inserted = false;
        for (const uint64_t child : nodes_[parent].children) {
            bool dropped = false;
            for (const uint64_t id : tabs) {
                if (child == id)
                    dropped = true;
            }
            if (!dropped) {
                kept.push_back(child);
                continue;
            }
            if (!inserted) {
                kept.push_back(nodes_[index].id);
                inserted = true;
            }
        }
        if (!inserted)
            kept.push_back(nodes_[index].id);
        nodes_[parent].children = std::move(kept);
    }

    /// Gives every loose run of tabs a tab list.
    void group_loose_tabs() {
        const std::size_t count = nodes_.size();
        std::vector<std::size_t> parents;
        for (std::size_t index = 0; index < count; ++index) {
            if (nodes_[index].role == bridge::Role::tab_list)
                continue;
            int tabs = 0;
            for (const uint64_t child : nodes_[index].children) {
                const bridge::Node* node = at(child);
                if (node != nullptr && node->role == bridge::Role::tab)
                    ++tabs;
            }
            if (tabs >= 2)
                parents.push_back(index);
        }
        for (const std::size_t parent : parents)
            group_tabs_under(parent);
    }

    /// Controls that overlap along one axis, joined through any control that
    /// overlaps two of them.
    struct Band {
        double start{};            ///< the least top (rows) or left edge (columns)
        bool have_start{};         ///< start has been set
        std::size_t first{};       ///< the place of its first control in the input
        std::vector<uint64_t> ids; ///< its controls, in input order
    };

    /// Splits ids into bands of overlapping rows or columns, sorted by where
    /// each starts.
    ///
    /// @param ids the child ids
    /// @param by_rows true for rows (vertical overlap), false for columns
    /// @return the bands
    std::vector<Band> bands_of(const std::vector<uint64_t>& ids, bool by_rows) const {
        const std::size_t count = ids.size();
        std::vector<std::size_t> link(count);
        for (std::size_t index = 0; index < count; ++index)
            link[index] = index;
        const auto find = [&](std::size_t index) {
            while (link[index] != index)
                index = link[index];
            return index;
        };
        for (std::size_t left = 0; left < count; ++left) {
            const bridge::Node* a = at(ids[left]);
            if (a == nullptr)
                continue;
            for (std::size_t right = left + 1; right < count; ++right) {
                const bridge::Node* b = at(ids[right]);
                if (b == nullptr)
                    continue;
                if (by_rows ? rows_overlap(*a, *b) : columns_overlap(*a, *b))
                    link[find(right)] = find(left);
            }
        }
        std::vector<Band> bands;
        std::unordered_map<std::size_t, std::size_t> band_of;
        for (std::size_t index = 0; index < count; ++index) {
            const std::size_t root = find(index);
            const auto found = band_of.find(root);
            std::size_t slot = 0;
            if (found == band_of.end()) {
                slot = bands.size();
                band_of.emplace(root, slot);
                Band band;
                band.first = index;
                bands.push_back(std::move(band));
            } else {
                slot = found->second;
            }
            bands[slot].ids.push_back(ids[index]);
            const bridge::Node* node = at(ids[index]);
            if (node == nullptr)
                continue;
            const double edge = by_rows ? node->y : node->x;
            if (!bands[slot].have_start || edge < bands[slot].start) {
                bands[slot].start = edge;
                bands[slot].have_start = true;
            }
        }
        std::stable_sort(bands.begin(), bands.end(), [](const Band& left, const Band& right) {
            if (left.start != right.start)
                return left.start < right.start;
            return left.first < right.first;
        });
        return bands;
    }

    /// Orders ids by bands of overlapping rows, top to bottom, and each band
    /// by its columns.
    ///
    /// A tall control overlaps every row it crosses, so those rows are one
    /// band and a column is read whole before the control beside it. Inside
    /// that column the rows stay rows: a label is read with the button on
    /// its own row, not with the labels below it that share its left edge.
    ///
    /// @param[in,out] children the child ids
    void order_by_rows(std::vector<uint64_t>& children) const {
        std::vector<Band> row_bands = bands_of(children, true);
        children.clear();
        for (Band& band : row_bands) {
            order_columns(band.ids);
            children.insert(children.end(), band.ids.begin(), band.ids.end());
        }
    }

    /// Orders one band of rows by its columns, left to right, each column by
    /// its own rows. A band that is one column is read top to bottom.
    ///
    /// @param[in,out] ids the band's ids
    void order_columns(std::vector<uint64_t>& ids) const {
        if (ids.size() < 2)
            return;
        std::vector<Band> column_bands = bands_of(ids, false);
        if (column_bands.size() < 2) {
            std::stable_sort(ids.begin(), ids.end(), [&](uint64_t left_id, uint64_t right_id) {
                const bridge::Node* left = at(left_id);
                const bridge::Node* right = at(right_id);
                if (left == nullptr || right == nullptr)
                    return left != nullptr;
                if (left->y != right->y)
                    return left->y < right->y;
                return left->x < right->x;
            });
            return;
        }
        ids.clear();
        for (Band& band : column_bands) {
            // Each column is fewer controls than the band, so this ends.
            order_by_rows(band.ids);
            ids.insert(ids.end(), band.ids.begin(), band.ids.end());
        }
    }

    /// Orders one parent's children by the screen's Tab order, or by place,
    /// or by overlapping rows.
    ///
    /// Where any child has a Tab index, those children follow it and the
    /// others slot in by where they sit. Where every child has a place and
    /// none has a Tab index, the place wins. Otherwise a column is read whole.
    ///
    /// @param[in,out] children the child ids
    void order_one(std::vector<uint64_t>& children) const {
        if (children.size() < 2)
            return;
        bool any_tab = false;
        bool all_placed = true;
        for (const uint64_t child : children) {
            const bridge::Node* node = at(child);
            if (node == nullptr || node->place <= 0)
                all_placed = false;
            if (node != nullptr && node->tab_index > 0)
                any_tab = true;
        }
        if (any_tab) {
            std::vector<uint64_t> positioned = children;
            order_by_rows(positioned);
            std::unordered_map<uint64_t, int> rank;
            for (std::size_t index = 0; index < positioned.size(); ++index)
                rank.emplace(positioned[index], static_cast<int>(index));
            std::vector<uint64_t> indexed;
            std::vector<uint64_t> plain;
            for (const uint64_t child : children) {
                const bridge::Node* node = at(child);
                if (node != nullptr && node->tab_index > 0)
                    indexed.push_back(child);
                else
                    plain.push_back(child);
            }
            std::stable_sort(
                indexed.begin(), indexed.end(), [&](uint64_t left_id, uint64_t right_id) {
                    const bridge::Node* left = at(left_id);
                    const bridge::Node* right = at(right_id);
                    const int32_t left_tab = left != nullptr ? left->tab_index : 0;
                    const int32_t right_tab = right != nullptr ? right->tab_index : 0;
                    return left_tab < right_tab;
                }
            );
            std::stable_sort(plain.begin(), plain.end(), [&](uint64_t left_id, uint64_t right_id) {
                return rank[left_id] < rank[right_id];
            });
            std::vector<uint64_t> ordered;
            std::size_t cursor = 0;
            int previous = -1;
            for (const uint64_t id : indexed) {
                const int here = rank[id];
                while (cursor < plain.size() && rank[plain[cursor]] > previous &&
                       rank[plain[cursor]] < here)
                    ordered.push_back(plain[cursor++]);
                ordered.push_back(id);
                previous = here;
            }
            while (cursor < plain.size())
                ordered.push_back(plain[cursor++]);
            children = std::move(ordered);
            return;
        }
        if (all_placed) {
            std::stable_sort(
                children.begin(), children.end(), [&](uint64_t left_id, uint64_t right_id) {
                    const bridge::Node* left = at(left_id);
                    const bridge::Node* right = at(right_id);
                    if (left == nullptr || right == nullptr)
                        return left != nullptr;
                    return left->place < right->place;
                }
            );
            return;
        }
        order_by_rows(children);
    }

    /// Orders every parent's children.
    void order_children() {
        for (bridge::Node& node : nodes_)
            order_one(node.children);
    }

    /// A focused control that was left out is heard as the visible button
    /// with its name or its label, when nothing visible is focused.
    void hand_covered_focus() {
        if (!covered_.set)
            return;
        for (const bridge::Node& node : nodes_) {
            if (node.focused && !node.hidden && node.role != bridge::Role::none)
                return;
        }
        for (bridge::Node& node : nodes_) {
            if (node.role != bridge::Role::button || node.label.empty())
                continue;
            if (node.label == covered_.name || node.label == covered_.label) {
                node.focused = true;
                return;
            }
        }
    }

    /// The id the reader is told holds the focus. A tab list, a radio group
    /// or a list that holds the focus announces its chosen child.
    ///
    /// @return the id; the window when nothing is focused
    uint64_t focused_id() const {
        uint64_t focus = bridge::window_node;
        bridge::Role role = bridge::Role::window;
        std::vector<uint64_t> children;
        for (const bridge::Node& node : nodes_) {
            if (!node.focused || node.hidden || node.role == bridge::Role::none)
                continue;
            focus = node.id;
            role = node.role;
            children = node.children;
            break;
        }
        if (role != bridge::Role::tab_list && role != bridge::Role::radio_group &&
            role != bridge::Role::list)
            return focus;
        for (const uint64_t child : children) {
            const bridge::Node* node = at(child);
            if (node != nullptr && node->selected_known && node->selected)
                return node->id;
        }
        return focus;
    }

    /// The nodes that can be reached from the window.
    ///
    /// @return those nodes
    std::vector<bridge::Node> reachable() const {
        std::vector<char> keep(nodes_.size(), 0);
        std::vector<std::size_t> pending;
        pending.push_back(0);
        keep[0] = 1;
        while (!pending.empty()) {
            const std::size_t current = pending.back();
            pending.pop_back();
            for (const uint64_t child : nodes_[current].children) {
                const auto found = id_at_.find(child);
                if (found == id_at_.end() || keep[found->second] != 0)
                    continue;
                keep[found->second] = 1;
                pending.push_back(found->second);
            }
        }
        std::vector<bridge::Node> kept;
        kept.reserve(nodes_.size());
        for (std::size_t index = 0; index < nodes_.size(); ++index) {
            if (keep[index] != 0)
                kept.push_back(nodes_[index]);
        }
        return kept;
    }

    const automation::WindowPlacement& placement_;
    bool keep_menu_overlay_;
    CoveredFocus covered_{};
    std::vector<bridge::Node> nodes_;
    std::unordered_map<uint64_t, std::size_t> id_at_;
    mutable std::unordered_set<uint64_t> used_;
    mutable std::unordered_map<std::string, uint32_t> seen_;
};

/// Reports whether the main-menu overlay is showing.
///
/// @param listing the controls
/// @return false when a screen package other than the main menu is showing
bool menu_overlay_kept(const automation::ControlListing& listing) {
    for (const automation::ControlNode& root : listing.tree.roots) {
        if (root.group && root.parent.kind == automation::ControlParentKind::screen_package &&
            root.parent.name != kMainMenu)
            return false;
    }
    return true;
}

} // namespace

namespace oa::app {

std::vector<oa::platform::screen_reader::Node> nodes_of(
    const oa::app::automation::ControlListing& listing,
    const oa::app::automation::WindowPlacement& placement,
    uint64_t& focus
) {
    TreeBuilder builder(placement, menu_overlay_kept(listing));
    for (const oa::app::automation::ControlNode& root : listing.tree.roots)
        builder.add_root(root);
    return builder.finish(focus);
}

} // namespace oa::app

#endif
