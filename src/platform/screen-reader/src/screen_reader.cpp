// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/platform/screen_reader.hpp"

#if !defined(OA_SCREEN_READER_COMPILED_OUT)

#include "oa/platform/screen_reader/api.hpp"

#include <SDL3/SDL.h>

#include <cstddef>

#if defined(_WIN32)
#include <string>
#include <windows.h>
#endif

namespace oa::platform::screen_reader {

namespace {

Calls bound{};
bool functions_ready = false;

#if defined(_WIN32)
HMODULE loaded_library = nullptr;

struct WindowsAdapter {
    accesskit_windows_subclassing_adapter* (*adapter_new)(
        HWND window,
        accesskit_activation_handler_callback activation,
        void* activation_data,
        accesskit_action_handler_callback action,
        void* action_data
    ) = nullptr;
    void (*adapter_free)(accesskit_windows_subclassing_adapter* adapter) = nullptr;
    accesskit_windows_queued_events* (*update_if_active)(
        accesskit_windows_subclassing_adapter* adapter,
        accesskit_tree_update_factory factory,
        void* factory_data
    ) = nullptr;
    void (*raise_events)(accesskit_windows_queued_events* events) = nullptr;
    accesskit_windows_subclassing_adapter* adapter = nullptr;
};

WindowsAdapter windows{};

/// The system's own version. A compatibility setting does not change it.
///
/// @return true on Windows 10 and later
bool windows_10_or_later() {
    using VersionFunction = LONG(WINAPI*)(OSVERSIONINFOW*);
    const HMODULE system_library = GetModuleHandleW(L"ntdll.dll");
    if (system_library == nullptr)
        return false;
    const auto read_version = reinterpret_cast<VersionFunction>(
        reinterpret_cast<void*>(GetProcAddress(system_library, "RtlGetVersion"))
    );
    if (read_version == nullptr)
        return false;
    OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof info;
    if (read_version(&info) != 0)
        return false;
    return info.dwMajorVersion >= 10;
}

/// accesskit.dll beside this program.
///
/// @return the path, or empty when the program's folder cannot be read
std::wstring dll_beside_program() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || static_cast<std::size_t>(length) >= path.size())
        return {};
    path.resize(static_cast<std::size_t>(length));
    const auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return {};
    path.resize(slash + 1);
    path += L"accesskit.dll";
    return path;
}

template <typename Function>
bool bind_symbol(const char* name, Function& slot) {
    slot =
        reinterpret_cast<Function>(reinterpret_cast<void*>(GetProcAddress(loaded_library, name)));
    return slot != nullptr;
}
#endif

/// Builds the window node and the test node.
///
/// @return the tree update; null only when the update itself cannot be made
accesskit_tree_update* initial_tree(void* /*userdata*/) {
    accesskit_node* window = bound.node_new(ACCESSKIT_ROLE_WINDOW);
    accesskit_node* label = bound.node_new(ACCESSKIT_ROLE_LABEL);
    if (label != nullptr)
        bound.node_set_label(label, test_label);
    if (window != nullptr)
        bound.node_push_child(window, test_node);
    accesskit_tree_update* update = bound.tree_update_with_capacity_and_focus(2, test_node);
    if (update == nullptr) {
        if (label != nullptr)
            bound.node_free(label);
        if (window != nullptr)
            bound.node_free(window);
        return nullptr;
    }
    accesskit_tree_info* info = bound.tree_info_new(window_node);
    if (info != nullptr)
        bound.tree_update_set_tree_info(update, info);
    if (label != nullptr)
        bound.tree_update_push_node(update, test_node, label);
    if (window != nullptr)
        bound.tree_update_push_node(update, window_node, window);
    return update;
}

/// Frees the action request. The actions themselves are answered later.
void on_action(accesskit_action_request* request, void* /*userdata*/) {
    if (bound.action_request_free != nullptr)
        bound.action_request_free(request);
}

#if defined(ACCESSKIT_IOS) || defined(__linux__) || defined(__DragonFly__) ||                      \
    defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
void on_deactivate(void* /*userdata*/) {
}
#endif

bool data_bound() {
    return bound.node_new != nullptr && bound.node_free != nullptr &&
           bound.node_set_label != nullptr && bound.node_role != nullptr &&
           bound.node_label != nullptr && bound.node_push_child != nullptr &&
           bound.string_free != nullptr && bound.tree_info_new != nullptr &&
           bound.tree_update_with_capacity_and_focus != nullptr &&
           bound.tree_update_set_tree_info != nullptr && bound.tree_update_push_node != nullptr &&
           bound.tree_update_free != nullptr && bound.action_request_free != nullptr;
}

#if !defined(_WIN32)
void bind_data() {
    bound.node_new = accesskit_node_new;
    bound.node_free = accesskit_node_free;
    bound.node_set_label = accesskit_node_set_label;
    bound.node_role = accesskit_node_role;
    bound.node_label = accesskit_node_label;
    bound.node_push_child = accesskit_node_push_child;
    bound.string_free = accesskit_string_free;
    bound.tree_info_new = accesskit_tree_info_new;
    bound.tree_update_with_capacity_and_focus = accesskit_tree_update_with_capacity_and_focus;
    bound.tree_update_set_tree_info = accesskit_tree_update_set_tree_info;
    bound.tree_update_push_node = accesskit_tree_update_push_node;
    bound.tree_update_free = accesskit_tree_update_free;
    bound.action_request_free = accesskit_action_request_free;
}
#endif

#if defined(ACCESSKIT_MACOS)
struct MacAdapter {
    void (*add_focus_forwarder)(const char* class_name) = nullptr;
    accesskit_macos_subclassing_adapter* (*adapter_for_window)(
        void* window,
        accesskit_activation_handler_callback activation,
        void* activation_data,
        accesskit_action_handler_callback action,
        void* action_data
    ) = nullptr;
    void (*adapter_free)(accesskit_macos_subclassing_adapter* adapter) = nullptr;
    accesskit_macos_queued_events* (*update_if_active)(
        accesskit_macos_subclassing_adapter* adapter,
        accesskit_tree_update_factory factory,
        void* factory_data
    ) = nullptr;
    accesskit_macos_queued_events* (*update_focus)(
        accesskit_macos_subclassing_adapter* adapter, bool focused
    ) = nullptr;
    void (*raise_events)(accesskit_macos_queued_events* events) = nullptr;
    accesskit_macos_subclassing_adapter* adapter = nullptr;
    bool forwarder_added = false;
};

MacAdapter mac{};
#endif

#if defined(ACCESSKIT_IOS)
struct IosAdapter {
    accesskit_ios_subclassing_adapter* (*adapter_for_window)(
        void* window,
        accesskit_activation_handler_callback activation,
        void* activation_data,
        accesskit_action_handler_callback action,
        void* action_data,
        accesskit_deactivation_handler_callback deactivation,
        void* deactivation_data
    ) = nullptr;
    void (*adapter_free)(accesskit_ios_subclassing_adapter* adapter) = nullptr;
    accesskit_ios_queued_events* (*update_if_active)(
        accesskit_ios_subclassing_adapter* adapter,
        accesskit_tree_update_factory factory,
        void* factory_data
    ) = nullptr;
    void (*raise_events)(accesskit_ios_queued_events* events) = nullptr;
    accesskit_ios_subclassing_adapter* adapter = nullptr;
};

IosAdapter ios{};
#endif

#if defined(__linux__) || defined(__DragonFly__) || defined(__FreeBSD__) || defined(__NetBSD__) || \
    defined(__OpenBSD__)
struct UnixAdapter {
    accesskit_unix_adapter* (*adapter_new)(
        accesskit_activation_handler_callback activation,
        void* activation_data,
        accesskit_action_handler_callback action,
        void* action_data,
        accesskit_deactivation_handler_callback deactivation,
        void* deactivation_data
    ) = nullptr;
    void (*adapter_free)(accesskit_unix_adapter* adapter) = nullptr;
    void (*update_if_active)(
        accesskit_unix_adapter* adapter, accesskit_tree_update_factory factory, void* factory_data
    ) = nullptr;
    void (*update_focus)(accesskit_unix_adapter* adapter, bool focused) = nullptr;
    void (*set_bounds)(
        accesskit_unix_adapter* adapter, accesskit_rect outer, accesskit_rect inner
    ) = nullptr;
    accesskit_unix_adapter* adapter = nullptr;
};

UnixAdapter unix_adapter{};
#endif

} // namespace

bool compiled_in() {
    return true;
}

const Calls* calls() {
    return functions_ready ? &bound : nullptr;
}

LoadResult load() {
    if (functions_ready)
        return LoadResult::ready;
#if defined(_WIN32)
    if (!windows_10_or_later())
        return LoadResult::old_windows;
    const std::wstring dll_path = dll_beside_program();
    if (dll_path.empty())
        return LoadResult::unavailable;
    if (GetFileAttributesW(dll_path.c_str()) == INVALID_FILE_ATTRIBUTES)
        return LoadResult::missing_library;
    const UINT previous_mode = SetErrorMode(SEM_FAILCRITICALERRORS);
    loaded_library = LoadLibraryW(dll_path.c_str());
    SetErrorMode(previous_mode);
    if (loaded_library == nullptr)
        return LoadResult::unavailable;
    const bool bound_data =
        bind_symbol("accesskit_node_new", bound.node_new) &&
        bind_symbol("accesskit_node_free", bound.node_free) &&
        bind_symbol("accesskit_node_set_label", bound.node_set_label) &&
        bind_symbol("accesskit_node_role", bound.node_role) &&
        bind_symbol("accesskit_node_label", bound.node_label) &&
        bind_symbol("accesskit_node_push_child", bound.node_push_child) &&
        bind_symbol("accesskit_string_free", bound.string_free) &&
        bind_symbol("accesskit_tree_info_new", bound.tree_info_new) &&
        bind_symbol(
            "accesskit_tree_update_with_capacity_and_focus",
            bound.tree_update_with_capacity_and_focus
        ) &&
        bind_symbol("accesskit_tree_update_set_tree_info", bound.tree_update_set_tree_info) &&
        bind_symbol("accesskit_tree_update_push_node", bound.tree_update_push_node) &&
        bind_symbol("accesskit_tree_update_free", bound.tree_update_free) &&
        bind_symbol("accesskit_action_request_free", bound.action_request_free) &&
        bind_symbol("accesskit_windows_subclassing_adapter_new", windows.adapter_new) &&
        bind_symbol("accesskit_windows_subclassing_adapter_free", windows.adapter_free) &&
        bind_symbol(
            "accesskit_windows_subclassing_adapter_update_if_active", windows.update_if_active
        ) &&
        bind_symbol("accesskit_windows_queued_events_raise", windows.raise_events);
    if (!bound_data || !data_bound()) {
        FreeLibrary(loaded_library);
        loaded_library = nullptr;
        return LoadResult::unavailable;
    }
#else
    bind_data();
#if defined(ACCESSKIT_MACOS)
    mac.add_focus_forwarder = accesskit_macos_add_focus_forwarder_to_window_class;
    mac.adapter_for_window = accesskit_macos_subclassing_adapter_for_window;
    mac.adapter_free = accesskit_macos_subclassing_adapter_free;
    mac.update_if_active = accesskit_macos_subclassing_adapter_update_if_active;
    mac.update_focus = accesskit_macos_subclassing_adapter_update_view_focus_state;
    mac.raise_events = accesskit_macos_queued_events_raise;
#elif defined(ACCESSKIT_IOS)
    ios.adapter_for_window = accesskit_ios_subclassing_adapter_for_window;
    ios.adapter_free = accesskit_ios_subclassing_adapter_free;
    ios.update_if_active = accesskit_ios_subclassing_adapter_update_if_active;
    ios.raise_events = accesskit_ios_queued_events_raise;
#elif defined(__linux__) || defined(__DragonFly__) || defined(__FreeBSD__) ||                      \
    defined(__NetBSD__) || defined(__OpenBSD__)
    unix_adapter.adapter_new = accesskit_unix_adapter_new;
    unix_adapter.adapter_free = accesskit_unix_adapter_free;
    unix_adapter.update_if_active = accesskit_unix_adapter_update_if_active;
    unix_adapter.update_focus = accesskit_unix_adapter_update_window_focus_state;
    unix_adapter.set_bounds = accesskit_unix_adapter_set_root_window_bounds;
#endif
    if (!data_bound())
        return LoadResult::unavailable;
#endif
    functions_ready = true;
    return LoadResult::ready;
}

bool attach(SDL_Window* window) {
    if (window == nullptr || load() != LoadResult::ready)
        return false;
    detach();
#if defined(_WIN32)
    const SDL_PropertiesID properties = SDL_GetWindowProperties(window);
    auto* window_handle = static_cast<HWND>(
        SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr)
    );
    if (window_handle == nullptr || windows.adapter_new == nullptr)
        return false;
    // AccessKit aborts when the window is already visible. Refuse that here.
    if ((SDL_GetWindowFlags(window) & SDL_WINDOW_HIDDEN) == 0)
        return false;
    windows.adapter = windows.adapter_new(window_handle, initial_tree, nullptr, on_action, nullptr);
    if (windows.adapter == nullptr)
        return false;
    accesskit_windows_queued_events* events =
        windows.update_if_active(windows.adapter, initial_tree, nullptr);
    if (events != nullptr)
        windows.raise_events(events);
    return true;
#elif defined(ACCESSKIT_MACOS)
    const SDL_PropertiesID properties = SDL_GetWindowProperties(window);
    void* native =
        SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
    // SDL does not report the content view. A missing native window is the
    // refusal it allows; the caller keeps the content view in place.
    if (native == nullptr || mac.adapter_for_window == nullptr)
        return false;
    if (!mac.forwarder_added && mac.add_focus_forwarder != nullptr) {
        mac.add_focus_forwarder("SDL3Window");
        mac.forwarder_added = true;
    }
    mac.adapter = mac.adapter_for_window(native, initial_tree, nullptr, on_action, nullptr);
    if (mac.adapter == nullptr)
        return false;
    const bool focused = (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
    accesskit_macos_queued_events* focus_events = mac.update_focus(mac.adapter, focused);
    if (focus_events != nullptr)
        mac.raise_events(focus_events);
    accesskit_macos_queued_events* events =
        mac.update_if_active(mac.adapter, initial_tree, nullptr);
    if (events != nullptr)
        mac.raise_events(events);
    return true;
#elif defined(ACCESSKIT_IOS)
    const SDL_PropertiesID properties = SDL_GetWindowProperties(window);
    void* native =
        SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr);
    // SDL does not report the root view controller. A missing native window
    // is the refusal it allows; the caller keeps that controller in place.
    if (native == nullptr || ios.adapter_for_window == nullptr)
        return false;
    ios.adapter = ios.adapter_for_window(
        native, initial_tree, nullptr, on_action, nullptr, on_deactivate, nullptr
    );
    if (ios.adapter == nullptr)
        return false;
    accesskit_ios_queued_events* events = ios.update_if_active(ios.adapter, initial_tree, nullptr);
    if (events != nullptr)
        ios.raise_events(events);
    return true;
#elif defined(__linux__) || defined(__DragonFly__) || defined(__FreeBSD__) ||                      \
    defined(__NetBSD__) || defined(__OpenBSD__)
    if (unix_adapter.adapter_new == nullptr)
        return false;
    unix_adapter.adapter =
        unix_adapter.adapter_new(initial_tree, nullptr, on_action, nullptr, on_deactivate, nullptr);
    if (unix_adapter.adapter == nullptr)
        return false;
    int width = 0;
    int height = 0;
    SDL_GetWindowSize(window, &width, &height);
    const accesskit_rect bounds{0, 0, static_cast<double>(width), static_cast<double>(height)};
    unix_adapter.set_bounds(unix_adapter.adapter, bounds, bounds);
    const bool focused = (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
    unix_adapter.update_focus(unix_adapter.adapter, focused);
    unix_adapter.update_if_active(unix_adapter.adapter, initial_tree, nullptr);
    return true;
#else
    return false;
#endif
}

void detach() {
#if defined(_WIN32)
    if (windows.adapter != nullptr && windows.adapter_free != nullptr)
        windows.adapter_free(windows.adapter);
    windows.adapter = nullptr;
#elif defined(ACCESSKIT_MACOS)
    if (mac.adapter != nullptr && mac.adapter_free != nullptr)
        mac.adapter_free(mac.adapter);
    mac.adapter = nullptr;
#elif defined(ACCESSKIT_IOS)
    if (ios.adapter != nullptr && ios.adapter_free != nullptr)
        ios.adapter_free(ios.adapter);
    ios.adapter = nullptr;
#elif defined(__linux__) || defined(__DragonFly__) || defined(__FreeBSD__) ||                      \
    defined(__NetBSD__) || defined(__OpenBSD__)
    if (unix_adapter.adapter != nullptr && unix_adapter.adapter_free != nullptr)
        unix_adapter.adapter_free(unix_adapter.adapter);
    unix_adapter.adapter = nullptr;
#endif
}

} // namespace oa::platform::screen_reader

#else

namespace oa::platform::screen_reader {

bool compiled_in() {
    return false;
}

LoadResult load() {
    return LoadResult::compiled_out;
}

bool attach(SDL_Window* /*window*/) {
    return false;
}

void detach() {
}

} // namespace oa::platform::screen_reader

#endif
