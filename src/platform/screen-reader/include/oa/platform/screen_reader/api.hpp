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
};

/// The functions load() bound.
///
/// @return the table after load() returned ready; null before that
[[nodiscard]] const Calls* calls();

} // namespace oa::platform::screen_reader
