// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Builds an AccessKit tree update through the C API and reads a node's role
// and label back. On Windows the functions are loaded from accesskit.dll
// beside this program. A Windows 95 build checks that the bridge is left out.

#include "oa/platform/screen_reader.hpp"

#include "oa/test/check.hpp"

#include <cstring>

#if !defined(OA_SCREEN_READER_COMPILED_OUT)
#include "oa/platform/screen_reader/api.hpp"
#endif

#if defined(_WIN32) && !defined(OA_SCREEN_READER_COMPILED_OUT)
#include <windows.h>
#endif

namespace {

namespace screen = oa::platform::screen_reader;

#if !defined(OA_SCREEN_READER_COMPILED_OUT)
/// The test node is a label, and the window node owns it, both read back
/// before the tree update takes the nodes.
void role_and_label_round_trip() {
    const screen::Calls* table = screen::calls();
    if (table == nullptr) {
        OA_CHECK(table != nullptr);
        return;
    }
    accesskit_node* label = table->node_new(ACCESSKIT_ROLE_LABEL);
    if (label == nullptr) {
        OA_CHECK(label != nullptr);
        return;
    }
    table->node_set_label(label, screen::test_label);
    OA_CHECK(table->node_role(label) == ACCESSKIT_ROLE_LABEL);
    char* text = table->node_label(label);
    OA_CHECK(text != nullptr && std::strcmp(text, screen::test_label) == 0);
    if (text != nullptr)
        table->string_free(text);

    accesskit_node* window = table->node_new(ACCESSKIT_ROLE_WINDOW);
    if (window == nullptr) {
        OA_CHECK(window != nullptr);
        table->node_free(label);
        return;
    }
    OA_CHECK(table->node_role(window) == ACCESSKIT_ROLE_WINDOW);
    table->node_push_child(window, screen::test_node);

    accesskit_tree_update* update =
        table->tree_update_with_capacity_and_focus(2, screen::test_node);
    if (update == nullptr) {
        OA_CHECK(update != nullptr);
        table->node_free(label);
        table->node_free(window);
        return;
    }
    accesskit_tree_info* info = table->tree_info_new(screen::window_node);
    OA_CHECK(info != nullptr);
    if (info != nullptr)
        table->tree_update_set_tree_info(update, info);
    table->tree_update_push_node(update, screen::test_node, label);
    table->tree_update_push_node(update, screen::window_node, window);
    table->tree_update_free(update);
}
#endif

} // namespace

int main() {
    const screen::LoadResult result = screen::load();
    if (result == screen::LoadResult::compiled_out) {
        OA_CHECK(!screen::compiled_in());
        return oa::test::check_exit_status();
    }
#if defined(_WIN32) && !defined(OA_SCREEN_READER_COMPILED_OUT)
    // The Visual Studio lane has the header and no DLL. An older Windows
    // does not load the DLL; speech speaks there instead.
    if (result == screen::LoadResult::missing_library)
        return 77;
    if (result == screen::LoadResult::old_windows) {
        OA_CHECK(screen::compiled_in());
        OA_CHECK(GetModuleHandleW(L"accesskit.dll") == nullptr);
        return oa::test::check_exit_status();
    }
#endif
    OA_CHECK(result == screen::LoadResult::ready);
#if !defined(OA_SCREEN_READER_COMPILED_OUT)
    if (result == screen::LoadResult::ready)
        role_and_label_round_trip();
#endif
    return oa::test::check_exit_status();
}
