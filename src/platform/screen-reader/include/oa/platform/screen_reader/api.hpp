// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The AccessKit C functions the bridge binds. A test builds a tree through
// these pointers, so a Windows build resolves them from the DLL instead of
// importing it.
#pragma once

#include "oa/platform/screen_reader.hpp"

// accesskit.h includes <windows.h> on Windows. Set these before that include
// so a file that includes this header does not see the min and max macros.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "accesskit.h"

#include <cstddef>

namespace oa::platform::screen_reader {

/// The C functions that build and read a node, bound by load().
struct Calls {
    /// Allocates a node of one role.
    accesskit_node* (*node_new)(accesskit_role role) = nullptr;
    /// Releases a node that no tree update owns.
    void (*node_free)(accesskit_node* node) = nullptr;
    /// Sets a node's label. The node copies the text.
    void (*node_set_label)(accesskit_node* node, const char* value) = nullptr;
    /// Reads a node's role.
    accesskit_role (*node_role)(const accesskit_node* node) = nullptr;
    /// Reads a node's label. The caller frees it with string_free.
    char* (*node_label)(const accesskit_node* node) = nullptr;
    /// Adds a child id to a node.
    void (*node_push_child)(accesskit_node* node, accesskit_node_id child) = nullptr;
    /// Releases a string AccessKit returned.
    void (*string_free)(char* text) = nullptr;
    /// Allocates the tree info whose root is one node.
    accesskit_tree_info* (*tree_info_new)(accesskit_node_id root) = nullptr;
    /// Allocates a tree update with room for nodes and a focused id.
    accesskit_tree_update* (*tree_update_with_capacity_and_focus)(
        size_t capacity, accesskit_node_id focus
    ) = nullptr;
    /// Gives a tree update its tree info. The update owns the info.
    void (*tree_update_set_tree_info)(accesskit_tree_update* update, accesskit_tree_info* tree) =
        nullptr;
    /// Adds a node to a tree update. The update owns the node.
    void (*tree_update_push_node)(
        accesskit_tree_update* update, accesskit_node_id id, accesskit_node* node
    ) = nullptr;
    /// Releases a tree update and the nodes it owns.
    void (*tree_update_free)(accesskit_tree_update* update) = nullptr;
    /// Releases an action request. The action callback must call it.
    void (*action_request_free)(accesskit_action_request* request) = nullptr;
    /// Sets a node's value. The node copies the text.
    void (*node_set_value)(accesskit_node* node, const char* value) = nullptr;
    /// Reads a node's value. The caller frees it with string_free.
    char* (*node_value)(const accesskit_node* node) = nullptr;
    /// Sets a node's description. The node copies the text.
    void (*node_set_description)(accesskit_node* node, const char* value) = nullptr;
    /// Reads a node's description. The caller frees it with string_free.
    char* (*node_description)(const accesskit_node* node) = nullptr;
    /// Sets a node's keyboard shortcut. The node copies the text.
    void (*node_set_keyboard_shortcut)(accesskit_node* node, const char* value) = nullptr;
    /// Reads a node's keyboard shortcut. The caller frees it with string_free.
    char* (*node_keyboard_shortcut)(const accesskit_node* node) = nullptr;
    /// Sets a node's bounds. The rectangle is the left, top, right and bottom edges.
    void (*node_set_bounds)(accesskit_node* node, accesskit_rect value) = nullptr;
    /// Reads a node's bounds.
    accesskit_opt_rect (*node_bounds)(const accesskit_node* node) = nullptr;
    /// Sets a node's numeric value.
    void (*node_set_numeric_value)(accesskit_node* node, double value) = nullptr;
    /// Reads a node's numeric value.
    accesskit_opt_double (*node_numeric_value)(const accesskit_node* node) = nullptr;
    /// Sets the least numeric value.
    void (*node_set_min_numeric_value)(accesskit_node* node, double value) = nullptr;
    /// Reads the least numeric value.
    accesskit_opt_double (*node_min_numeric_value)(const accesskit_node* node) = nullptr;
    /// Sets the greatest numeric value.
    void (*node_set_max_numeric_value)(accesskit_node* node, double value) = nullptr;
    /// Reads the greatest numeric value.
    accesskit_opt_double (*node_max_numeric_value)(const accesskit_node* node) = nullptr;
    /// Sets how far one step moves the numeric value.
    void (*node_set_numeric_value_step)(accesskit_node* node, double value) = nullptr;
    /// Reads how far one step moves the numeric value.
    accesskit_opt_double (*node_numeric_value_step)(const accesskit_node* node) = nullptr;
    /// Sets whether a check box or a switch is on.
    void (*node_set_toggled)(accesskit_node* node, accesskit_toggled value) = nullptr;
    /// Reads whether a check box or a switch is on.
    accesskit_opt_toggled (*node_toggled)(const accesskit_node* node) = nullptr;
    /// Sets whether the control is the chosen one.
    void (*node_set_selected)(accesskit_node* node, bool value) = nullptr;
    /// Reads whether the control is the chosen one.
    accesskit_opt_bool (*node_is_selected)(const accesskit_node* node) = nullptr;
    /// Sets whether a tree row is open.
    void (*node_set_expanded)(accesskit_node* node, bool value) = nullptr;
    /// Reads whether a tree row is open.
    accesskit_opt_bool (*node_is_expanded)(const accesskit_node* node) = nullptr;
    /// Marks a control disabled. A control that is not disabled is left unmarked.
    void (*node_set_disabled)(accesskit_node* node) = nullptr;
    /// Reads whether a control is disabled.
    bool (*node_is_disabled)(const accesskit_node* node) = nullptr;
    /// Sets the control's place in its set, from 1.
    void (*node_set_position_in_set)(accesskit_node* node, size_t value) = nullptr;
    /// Reads the control's place in its set.
    accesskit_opt_index (*node_position_in_set)(const accesskit_node* node) = nullptr;
    /// Sets how many share the control's set.
    void (*node_set_size_of_set)(accesskit_node* node, size_t value) = nullptr;
    /// Reads how many share the control's set.
    accesskit_opt_index (*node_size_of_set)(const accesskit_node* node) = nullptr;
    /// Sets how a change of the control is announced.
    void (*node_set_live)(accesskit_node* node, accesskit_live value) = nullptr;
    /// Reads how a change of the control is announced.
    accesskit_opt_live (*node_live)(const accesskit_node* node) = nullptr;
    /// Marks a dialog modal.
    void (*node_set_modal)(accesskit_node* node) = nullptr;
    /// Reads whether a dialog is modal.
    bool (*node_is_modal)(const accesskit_node* node) = nullptr;
    /// Adds an action the control takes.
    void (*node_add_action)(accesskit_node* node, accesskit_action action) = nullptr;
    /// Reads whether the control takes an action.
    bool (*node_supports_action)(const accesskit_node* node, accesskit_action action) = nullptr;
    /// Reads a node's child ids. The pointer is borrowed until the node is freed.
    accesskit_node_ids (*node_children)(const accesskit_node* node) = nullptr;
};

/// The functions load() bound.
///
/// @return the table after load() returned ready; null before that
[[nodiscard]] const Calls* calls();

} // namespace oa::platform::screen_reader
