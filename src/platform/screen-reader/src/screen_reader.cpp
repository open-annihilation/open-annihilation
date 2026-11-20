// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/platform/screen_reader.hpp"
#include "oa/platform/screen_reader/speech.hpp"

#include <string_view>

namespace oa::platform::screen_reader {

Preference preference_of(std::string_view text) {
    if (text == "on")
        return Preference::on;
    if (text == "off")
        return Preference::off;
    return Preference::automatic;
}

} // namespace oa::platform::screen_reader

#if !defined(OA_SCREEN_READER_COMPILED_OUT)

#include "oa/platform/screen_reader/announce.hpp"
#include "oa/platform/screen_reader/api.hpp"
#include "oa/platform/screen_reader/tree.hpp"

static_assert(
    static_cast<unsigned>(oa::platform::screen_reader::SpokenRole::none) ==
    static_cast<unsigned>(oa::platform::screen_reader::Role::none)
);
static_assert(
    static_cast<unsigned>(oa::platform::screen_reader::SpokenRole::button) ==
    static_cast<unsigned>(oa::platform::screen_reader::Role::button)
);
static_assert(
    static_cast<unsigned>(oa::platform::screen_reader::SpokenRole::label) ==
    static_cast<unsigned>(oa::platform::screen_reader::Role::label)
);
static_assert(
    static_cast<unsigned>(oa::platform::screen_reader::SpokenRole::window) ==
    static_cast<unsigned>(oa::platform::screen_reader::Role::window)
);

#include <SDL3/SDL.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#if defined(__linux__) || defined(__DragonFly__) || defined(__FreeBSD__) || defined(__NetBSD__) || \
    defined(__OpenBSD__)
#include <dlfcn.h>
#endif

namespace oa::platform::screen_reader {

#if defined(__APPLE__)
/// Whether VoiceOver is running. Defined in voice_over.mm.
bool voice_over_running();
#endif

namespace {

Calls bound{};
bool functions_ready = false;
Preference chosen_preference = Preference::automatic;
SDL_Window* attached_window = nullptr;
SDL_Window* skipped_window = nullptr;
bool attach_gave_up = false;
bool speech_needed = false;
std::atomic<bool> reader_active{false};
/// A test's stand-in for the adapter update. Null in the game.
bool (*update_hook)() = nullptr;
int shown_width = -1;
int shown_height = -1;
int shown_focus = -1;

#if defined(_WIN32)
HMODULE loaded_library = nullptr;

struct WindowsAdapter {
    accesskit_windows_subclassing_adapter* (*subclass_new)(
        HWND window,
        accesskit_activation_handler_callback activation,
        void* activation_data,
        accesskit_action_handler_callback action,
        void* action_data
    ) = nullptr;
    void (*subclass_free)(accesskit_windows_subclassing_adapter* adapter) = nullptr;
    accesskit_windows_queued_events* (*subclass_update)(
        accesskit_windows_subclassing_adapter* adapter,
        accesskit_tree_update_factory factory,
        void* factory_data
    ) = nullptr;
    accesskit_windows_adapter* (*plain_new)(
        HWND window, bool focused, accesskit_action_handler_callback action, void* action_data
    ) = nullptr;
    void (*plain_free)(accesskit_windows_adapter* adapter) = nullptr;
    accesskit_windows_queued_events* (*plain_update)(
        accesskit_windows_adapter* adapter,
        accesskit_tree_update_factory factory,
        void* factory_data
    ) = nullptr;
    accesskit_windows_queued_events* (*plain_focus)(
        accesskit_windows_adapter* adapter, bool focused
    ) = nullptr;
    accesskit_opt_lresult (*handle_wm_getobject)(
        accesskit_windows_adapter* adapter,
        WPARAM wparam,
        LPARAM lparam,
        accesskit_activation_handler_callback activation,
        void* activation_data
    ) = nullptr;
    void (*raise_events)(accesskit_windows_queued_events* events) = nullptr;
    accesskit_windows_subclassing_adapter* subclass = nullptr;
    accesskit_windows_adapter* plain = nullptr;
    HWND plain_window = nullptr;
    bool subclass_installed = false;
};

WindowsAdapter windows{};

/// comctl32's subclass, loaded by name so the import table stays free of it.
using ReaderSubclass = LRESULT(CALLBACK*)(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
using SetSubclass = BOOL(WINAPI*)(HWND, ReaderSubclass, UINT_PTR, DWORD_PTR);
using RemoveSubclass = BOOL(WINAPI*)(HWND, ReaderSubclass, UINT_PTR);
using DefSubclass = LRESULT(WINAPI*)(HWND, UINT, WPARAM, LPARAM);

constexpr UINT_PTR kReaderSubclass = 0x0A26;
HMODULE comctl_library = nullptr;
SetSubclass set_window_subclass = nullptr;
RemoveSubclass remove_window_subclass = nullptr;
DefSubclass def_subclass_proc = nullptr;

LRESULT CALLBACK reader_subclass(
    HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR subclass_id, DWORD_PTR data
);

/// Loads SetWindowSubclass and leaves comctl32 loaded.
///
/// @return true when the three calls are bound
bool subclass_hooks_ready() {
    if (set_window_subclass != nullptr && remove_window_subclass != nullptr &&
        def_subclass_proc != nullptr)
        return true;
    if (comctl_library == nullptr)
        comctl_library = LoadLibraryW(L"comctl32.dll");
    if (comctl_library == nullptr)
        return false;
    set_window_subclass = reinterpret_cast<SetSubclass>(
        reinterpret_cast<void*>(GetProcAddress(comctl_library, "SetWindowSubclass"))
    );
    remove_window_subclass = reinterpret_cast<RemoveSubclass>(
        reinterpret_cast<void*>(GetProcAddress(comctl_library, "RemoveWindowSubclass"))
    );
    def_subclass_proc = reinterpret_cast<DefSubclass>(
        reinterpret_cast<void*>(GetProcAddress(comctl_library, "DefSubclassProc"))
    );
    return set_window_subclass != nullptr && remove_window_subclass != nullptr &&
           def_subclass_proc != nullptr;
}

/// Whether Narrator says it is running. Narrator does not set the
/// screen-reader flag: it keeps its state where Settings reads it, under
/// HKEY_CURRENT_USER. Where Narrator keeps no such value, the answer is
/// false.
///
/// @return true while Narrator is running
bool narrator_running() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(
            HKEY_CURRENT_USER, L"Software\\Microsoft\\Narrator\\NoRoam", 0, KEY_QUERY_VALUE, &key
        ) != ERROR_SUCCESS)
        return false;
    DWORD state = 0;
    DWORD type = 0;
    DWORD size = sizeof state;
    const LONG read = RegQueryValueExW(
        key, L"RunningState", nullptr, &type, reinterpret_cast<BYTE*>(&state), &size
    );
    RegCloseKey(key);
    return read == ERROR_SUCCESS && type == REG_DWORD && size == sizeof state && state != 0;
}

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

/// The tree AccessKit is shown the moment a reader asks. The game's tree
/// follows on the game's thread. This does not touch the game.
///
/// @return the placeholder tree
accesskit_tree_update* on_activate(void* /*userdata*/) {
    reader_active.store(true, std::memory_order_release);
    return placeholder_update();
}

/// Queues the action and frees the request. The game's thread turns the
/// queue into keys and presses.
void on_action(accesskit_action_request* request, void* /*userdata*/) {
    enqueue_request(request);
    if (bound.action_request_free != nullptr)
        bound.action_request_free(request);
}

#if defined(ACCESSKIT_IOS) || defined(__linux__) || defined(__DragonFly__) ||                      \
    defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
void on_deactivate(void* /*userdata*/) {
    reader_active.store(false, std::memory_order_release);
    forget_sent_tree();
}
#endif

#if defined(_WIN32)
/// Drops the plain adapter's subclass. Safe when none is installed.
///
/// @param window the window, or null to use the one that was subclassed
void remove_plain_subclass(HWND window) {
    if (!windows.subclass_installed || remove_window_subclass == nullptr)
        return;
    const HWND target = window != nullptr ? window : windows.plain_window;
    if (target != nullptr)
        remove_window_subclass(target, reader_subclass, kReaderSubclass);
    windows.subclass_installed = false;
    windows.plain_window = nullptr;
}

LRESULT CALLBACK reader_subclass(
    HWND window,
    UINT message,
    WPARAM wparam,
    LPARAM lparam,
    UINT_PTR /*subclass_id*/,
    DWORD_PTR /*data*/
) {
    if (message == WM_NCDESTROY) {
        remove_plain_subclass(window);
        if (def_subclass_proc != nullptr)
            return def_subclass_proc(window, message, wparam, lparam);
        return 0;
    }
    if (windows.plain != nullptr && message == WM_GETOBJECT &&
        windows.handle_wm_getobject != nullptr) {
        const accesskit_opt_lresult result =
            windows.handle_wm_getobject(windows.plain, wparam, lparam, on_activate, nullptr);
        if (result.has_value)
            return result.value;
    }
    if (windows.plain != nullptr && windows.plain_focus != nullptr &&
        (message == WM_SETFOCUS || message == WM_KILLFOCUS)) {
        accesskit_windows_queued_events* events =
            windows.plain_focus(windows.plain, message == WM_SETFOCUS);
        if (events != nullptr && windows.raise_events != nullptr)
            windows.raise_events(events);
    }
    if (def_subclass_proc != nullptr)
        return def_subclass_proc(window, message, wparam, lparam);
    return 0;
}

/// Installs the subclass that routes the reader's messages.
///
/// @param window the game's window
/// @return true when the subclass is installed
bool install_plain_subclass(HWND window) {
    if (window == nullptr || !subclass_hooks_ready())
        return false;
    if (windows.subclass_installed && windows.plain_window == window)
        return true;
    if (set_window_subclass(window, reader_subclass, kReaderSubclass, 0) == FALSE)
        return false;
    windows.plain_window = window;
    windows.subclass_installed = true;
    return true;
}
#endif

bool data_bound() {
    return bound.node_new != nullptr && bound.node_free != nullptr &&
           bound.node_set_label != nullptr && bound.node_role != nullptr &&
           bound.node_label != nullptr && bound.node_push_child != nullptr &&
           bound.string_free != nullptr && bound.tree_info_new != nullptr &&
           bound.tree_update_with_capacity_and_focus != nullptr &&
           bound.tree_update_set_tree_info != nullptr && bound.tree_update_push_node != nullptr &&
           bound.tree_update_free != nullptr && bound.action_request_free != nullptr &&
           bound.node_set_value != nullptr && bound.node_value != nullptr &&
           bound.node_set_description != nullptr && bound.node_description != nullptr &&
           bound.node_set_keyboard_shortcut != nullptr && bound.node_keyboard_shortcut != nullptr &&
           bound.node_set_bounds != nullptr && bound.node_bounds != nullptr &&
           bound.node_set_numeric_value != nullptr && bound.node_numeric_value != nullptr &&
           bound.node_set_min_numeric_value != nullptr && bound.node_min_numeric_value != nullptr &&
           bound.node_set_max_numeric_value != nullptr && bound.node_max_numeric_value != nullptr &&
           bound.node_set_numeric_value_step != nullptr &&
           bound.node_numeric_value_step != nullptr && bound.node_set_toggled != nullptr &&
           bound.node_toggled != nullptr && bound.node_set_selected != nullptr &&
           bound.node_is_selected != nullptr && bound.node_set_expanded != nullptr &&
           bound.node_is_expanded != nullptr && bound.node_set_disabled != nullptr &&
           bound.node_is_disabled != nullptr && bound.node_set_position_in_set != nullptr &&
           bound.node_position_in_set != nullptr && bound.node_set_size_of_set != nullptr &&
           bound.node_size_of_set != nullptr && bound.node_set_live != nullptr &&
           bound.node_live != nullptr && bound.node_set_modal != nullptr &&
           bound.node_is_modal != nullptr && bound.node_add_action != nullptr &&
           bound.node_supports_action != nullptr && bound.node_children != nullptr;
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
    bound.node_set_value = accesskit_node_set_value;
    bound.node_value = accesskit_node_value;
    bound.node_set_description = accesskit_node_set_description;
    bound.node_description = accesskit_node_description;
    bound.node_set_keyboard_shortcut = accesskit_node_set_keyboard_shortcut;
    bound.node_keyboard_shortcut = accesskit_node_keyboard_shortcut;
    bound.node_set_bounds = accesskit_node_set_bounds;
    bound.node_bounds = accesskit_node_bounds;
    bound.node_set_numeric_value = accesskit_node_set_numeric_value;
    bound.node_numeric_value = accesskit_node_numeric_value;
    bound.node_set_min_numeric_value = accesskit_node_set_min_numeric_value;
    bound.node_min_numeric_value = accesskit_node_min_numeric_value;
    bound.node_set_max_numeric_value = accesskit_node_set_max_numeric_value;
    bound.node_max_numeric_value = accesskit_node_max_numeric_value;
    bound.node_set_numeric_value_step = accesskit_node_set_numeric_value_step;
    bound.node_numeric_value_step = accesskit_node_numeric_value_step;
    bound.node_set_toggled = accesskit_node_set_toggled;
    bound.node_toggled = accesskit_node_toggled;
    bound.node_set_selected = accesskit_node_set_selected;
    bound.node_is_selected = accesskit_node_is_selected;
    bound.node_set_expanded = accesskit_node_set_expanded;
    bound.node_is_expanded = accesskit_node_is_expanded;
    bound.node_set_disabled = accesskit_node_set_disabled;
    bound.node_is_disabled = accesskit_node_is_disabled;
    bound.node_set_position_in_set = accesskit_node_set_position_in_set;
    bound.node_position_in_set = accesskit_node_position_in_set;
    bound.node_set_size_of_set = accesskit_node_set_size_of_set;
    bound.node_size_of_set = accesskit_node_size_of_set;
    bound.node_set_live = accesskit_node_set_live;
    bound.node_live = accesskit_node_live;
    bound.node_set_modal = accesskit_node_set_modal;
    bound.node_is_modal = accesskit_node_is_modal;
    bound.node_add_action = accesskit_node_add_action;
    bound.node_supports_action = accesskit_node_supports_action;
    bound.node_children = accesskit_node_children;
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

#if defined(__linux__) || defined(__DragonFly__) || defined(__FreeBSD__) || defined(__NetBSD__) || \
    defined(__OpenBSD__)

/// Enough of DBusError for the calls below to write it. libdbus is not linked.
struct DBusError {
    const char* name = nullptr;
    const char* message = nullptr;
    unsigned int dummy1 : 1;
    unsigned int dummy2 : 1;
    unsigned int dummy3 : 1;
    unsigned int dummy4 : 1;
    unsigned int dummy5 : 1;
    void* padding1 = nullptr;
};

struct DBusConnection;
struct DBusMessage;
struct DBusPendingCall;

/// The iterator libdbus writes. Its layout is the one libdbus publishes.
struct DBusMessageIter {
    void* dummy1{};
    void* dummy2{};
    uint32_t dummy3{};
    int dummy4{};
    int dummy5{};
    int dummy6{};
    int dummy7{};
    int dummy8{};
    int dummy9{};
    int dummy10{};
    int dummy11{};
    int pad1{};
    void* pad2{};
    void* pad3{};
};

/// The libdbus calls this file loads. A null library means they are not there.
struct DBusCalls {
    void* library = nullptr;
    void (*error_init)(DBusError* error) = nullptr;
    void (*error_free)(DBusError* error) = nullptr;
    unsigned (*error_is_set)(const DBusError* error) = nullptr;
    DBusConnection* (*bus_get)(int type, DBusError* error) = nullptr;
    void (*set_exit)(DBusConnection* connection, unsigned exit_on_disconnect) = nullptr;
    void (*unref_connection)(DBusConnection* connection) = nullptr;
    DBusMessage* (*new_call)(
        const char* destination, const char* path, const char* interface, const char* method
    ) = nullptr;
    unsigned (*append_args)(DBusMessage* message, int first, ...) = nullptr;
    void (*unref_message)(DBusMessage* message) = nullptr;
    int (*message_type)(DBusMessage* message) = nullptr;
    unsigned (*iter_init)(DBusMessage* message, DBusMessageIter* iter) = nullptr;
    void (*iter_recurse)(DBusMessageIter* iter, DBusMessageIter* sub) = nullptr;
    int (*iter_type)(DBusMessageIter* iter) = nullptr;
    void (*iter_get_basic)(DBusMessageIter* iter, void* value) = nullptr;
    unsigned (*send_with_reply)(
        DBusConnection* connection, DBusMessage* message, DBusPendingCall** pending, int timeout
    ) = nullptr;
    unsigned (*read_write)(DBusConnection* connection, int timeout) = nullptr;
    unsigned (*read_write_dispatch)(DBusConnection* connection, int timeout) = nullptr;
    unsigned (*pending_completed)(DBusPendingCall* pending) = nullptr;
    DBusMessage* (*steal_reply)(DBusPendingCall* pending) = nullptr;
    void (*cancel_pending)(DBusPendingCall* pending) = nullptr;
    void (*unref_pending)(DBusPendingCall* pending) = nullptr;
};

template <typename Function>
bool dbus_symbol(void* library, const char* name, Function& slot) {
    slot = reinterpret_cast<Function>(dlsym(library, name));
    return slot != nullptr;
}

/// Loads libdbus once.
///
/// @return the calls, or null when the library is not there
const DBusCalls* dbus_calls() {
    static DBusCalls loaded;
    static bool tried = false;
    if (tried)
        return loaded.library != nullptr ? &loaded : nullptr;
    tried = true;
    loaded.library = dlopen("libdbus-1.so.3", RTLD_NOW | RTLD_LOCAL);
    if (loaded.library == nullptr)
        loaded.library = dlopen("libdbus-1.so", RTLD_NOW | RTLD_LOCAL);
    if (loaded.library == nullptr)
        return nullptr;
    const bool ready =
        dbus_symbol(loaded.library, "dbus_error_init", loaded.error_init) &&
        dbus_symbol(loaded.library, "dbus_error_free", loaded.error_free) &&
        dbus_symbol(loaded.library, "dbus_error_is_set", loaded.error_is_set) &&
        dbus_symbol(loaded.library, "dbus_bus_get", loaded.bus_get) &&
        dbus_symbol(loaded.library, "dbus_connection_set_exit_on_disconnect", loaded.set_exit) &&
        dbus_symbol(loaded.library, "dbus_connection_unref", loaded.unref_connection) &&
        dbus_symbol(loaded.library, "dbus_message_new_method_call", loaded.new_call) &&
        dbus_symbol(loaded.library, "dbus_message_append_args", loaded.append_args) &&
        dbus_symbol(loaded.library, "dbus_message_unref", loaded.unref_message) &&
        dbus_symbol(loaded.library, "dbus_message_get_type", loaded.message_type) &&
        dbus_symbol(loaded.library, "dbus_message_iter_init", loaded.iter_init) &&
        dbus_symbol(loaded.library, "dbus_message_iter_recurse", loaded.iter_recurse) &&
        dbus_symbol(loaded.library, "dbus_message_iter_get_arg_type", loaded.iter_type) &&
        dbus_symbol(loaded.library, "dbus_message_iter_get_basic", loaded.iter_get_basic) &&
        dbus_symbol(loaded.library, "dbus_connection_send_with_reply", loaded.send_with_reply) &&
        dbus_symbol(loaded.library, "dbus_connection_read_write", loaded.read_write) &&
        dbus_symbol(
            loaded.library, "dbus_connection_read_write_dispatch", loaded.read_write_dispatch
        ) &&
        dbus_symbol(loaded.library, "dbus_pending_call_get_completed", loaded.pending_completed) &&
        dbus_symbol(loaded.library, "dbus_pending_call_steal_reply", loaded.steal_reply) &&
        dbus_symbol(loaded.library, "dbus_pending_call_cancel", loaded.cancel_pending) &&
        dbus_symbol(loaded.library, "dbus_pending_call_unref", loaded.unref_pending);
    if (!ready) {
        dlclose(loaded.library);
        loaded.library = nullptr;
        return nullptr;
    }
    return &loaded;
}

/// Reads a Properties.Get reply whose value is a variant boolean.
///
/// @param dbus the loaded calls
/// @param reply the method return
/// @param[out] enabled the boolean, when the reply holds one
/// @return true when the reply holds a variant boolean
bool variant_bool(const DBusCalls* dbus, DBusMessage* reply, bool& enabled) {
    DBusMessageIter outer{};
    if (dbus->iter_init(reply, &outer) == 0)
        return false;
    if (dbus->iter_type(&outer) != static_cast<int>('v'))
        return false;
    DBusMessageIter inner{};
    dbus->iter_recurse(&outer, &inner);
    if (dbus->iter_type(&inner) != static_cast<int>('b'))
        return false;
    uint32_t value = 0;
    dbus->iter_get_basic(&inner, &value);
    enabled = value != 0;
    return true;
}

/// Sends Properties.Get for one org.a11y.Status property. The call does not wait.
///
/// @param dbus the loaded calls
/// @param bus the shared session connection
/// @param property IsEnabled or ScreenReaderEnabled
/// @param[out] pending the outstanding call; null when it was not sent
/// @return true when a call is outstanding
bool send_status(
    const DBusCalls* dbus, DBusConnection* bus, const char* property, DBusPendingCall*& pending
) {
    pending = nullptr;
    DBusMessage* call =
        dbus->new_call("org.a11y.Bus", "/org/a11y/bus", "org.freedesktop.DBus.Properties", "Get");
    if (call == nullptr)
        return false;
    const char* interface_name = "org.a11y.Status";
    const unsigned appended = dbus->append_args(
        call, static_cast<int>('s'), &interface_name, static_cast<int>('s'), &property, 0
    );
    unsigned sent = 0;
    if (appended != 0)
        sent = dbus->send_with_reply(bus, call, &pending, 200);
    dbus->unref_message(call);
    if (sent == 0)
        pending = nullptr;
    return pending != nullptr;
}

/// Dispatches already-queued messages without waiting.
///
/// A shared connection completes a pending call only when someone dispatches
/// it. Timeout 0 does not wait. A few steps cover the reply and signals that
/// are already queued, and a busy bus cannot hold the frame.
///
/// @param dbus the loaded calls
/// @param bus the shared session connection
/// @param pending the outstanding call
void drain_pending(const DBusCalls* dbus, DBusConnection* bus, DBusPendingCall* pending) {
    for (int step = 0; step < 8 && dbus->pending_completed(pending) == 0; ++step) {
        if (dbus->read_write_dispatch(bus, 0) == 0)
            break;
    }
}

/// Drops an outstanding call the bus did not answer.
///
/// @param dbus the loaded calls
/// @param pending the call; cleared
void drop_pending(const DBusCalls* dbus, DBusPendingCall*& pending) {
    if (pending == nullptr)
        return;
    dbus->cancel_pending(pending);
    dbus->unref_pending(pending);
    pending = nullptr;
}

/// Applies one Properties.Get reply. A boolean locks the property name.
///
/// @param dbus the loaded calls
/// @param reply the reply; freed here. Null is nothing
/// @param[in,out] enabled the last boolean
/// @param[in,out] locked true once a boolean has been read
/// @param[in,out] legacy true to ask ScreenReaderEnabled next
void apply_reply(
    const DBusCalls* dbus, DBusMessage* reply, bool& enabled, bool& locked, bool& legacy
) {
    if (reply == nullptr)
        return;
    // 2 is METHOD_RETURN. Anything else is an error, so the other property
    // name is tried until one of them returns a boolean.
    if (dbus->message_type(reply) == 2) {
        bool value = false;
        if (variant_bool(dbus, reply, value)) {
            enabled = value;
            locked = true;
        }
    } else if (!locked) {
        legacy = !legacy;
    }
    dbus->unref_message(reply);
}

/// Takes a completed call's reply and drops the call.
///
/// @param dbus the loaded calls
/// @param[in,out] pending the call; cleared when it has completed
/// @param[in,out] enabled the last boolean
/// @param[in,out] locked true once a boolean has been read
/// @param[in,out] legacy true to ask ScreenReaderEnabled next
void take_reply(
    const DBusCalls* dbus, DBusPendingCall*& pending, bool& enabled, bool& locked, bool& legacy
) {
    if (pending == nullptr || dbus->pending_completed(pending) == 0)
        return;
    DBusMessage* reply = dbus->steal_reply(pending);
    dbus->unref_pending(pending);
    pending = nullptr;
    apply_reply(dbus, reply, enabled, locked, legacy);
}

/// Asks AT-SPI whether a screen reader is enabled.
///
/// The question is Properties.Get of org.a11y.Status.IsEnabled on the shared
/// session connection, which is kept open. The reply is a variant boolean.
/// ScreenReaderEnabled is the older name, used when IsEnabled does not answer.
/// The call is flushed and dispatched without waiting, and a failed bus is not
/// asked again. Any failure is no reader.
///
/// @return true when the last answer said a reader is enabled
bool atspi_enabled() {
    const char* session = SDL_getenv("DBUS_SESSION_BUS_ADDRESS");
    if (session == nullptr || session[0] == '\0')
        return false;
    const DBusCalls* dbus = dbus_calls();
    if (dbus == nullptr)
        return false;

    static DBusConnection* bus = nullptr;
    static bool bus_failed = false;
    static DBusPendingCall* pending = nullptr;
    static Uint64 pending_at = 0;
    static bool enabled = false;
    static bool locked = false;
    static bool legacy = false;

    if (bus == nullptr) {
        if (bus_failed)
            return false;
        DBusError error{};
        dbus->error_init(&error);
        bus = dbus->bus_get(0, &error);
        if (bus == nullptr || dbus->error_is_set(&error) != 0) {
            if (bus != nullptr)
                dbus->unref_connection(bus);
            bus = nullptr;
            bus_failed = true;
            dbus->error_free(&error);
            return false;
        }
        // The shared connection's default is to abort the process on disconnect.
        dbus->set_exit(bus, 0);
        dbus->error_free(&error);
    }

    if (pending != nullptr) {
        drain_pending(dbus, bus, pending);
        if (dbus->pending_completed(pending) != 0)
            take_reply(dbus, pending, enabled, locked, legacy);
        else if (SDL_GetTicks() - pending_at > 200)
            // The call was flushed when it was sent. A later poll that still
            // has no reply gives up, so a stuck call cannot sit forever.
            drop_pending(dbus, pending);
    }

    if (pending == nullptr) {
        const char* property = legacy ? "ScreenReaderEnabled" : "IsEnabled";
        if (send_status(dbus, bus, property, pending)) {
            pending_at = SDL_GetTicks();
            // Put the call on the socket without waiting. A reply that is
            // already queued is taken now; otherwise the next poll reads it.
            dbus->read_write(bus, 0);
            drain_pending(dbus, bus, pending);
            if (dbus->pending_completed(pending) != 0)
                take_reply(dbus, pending, enabled, locked, legacy);
        }
    }
    return enabled;
}

#endif

/// Asks the system once, with no cache.
///
/// @return true while a screen reader is running
bool query_reader_running() {
#if defined(_WIN32)
    BOOL running = FALSE;
#ifndef SPI_GETSCREENREADER
    constexpr UINT screen_reader_flag = 0x0046;
#else
    constexpr UINT screen_reader_flag = SPI_GETSCREENREADER;
#endif
    if (SystemParametersInfoW(screen_reader_flag, 0, &running, 0) != FALSE && running != FALSE)
        return true;
    return narrator_running();
#elif defined(__APPLE__)
    return voice_over_running();
#elif defined(__linux__) || defined(__DragonFly__) || defined(__FreeBSD__) ||                      \
    defined(__NetBSD__) || defined(__OpenBSD__)
    return atspi_enabled();
#else
    return false;
#endif
}

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
        bind_symbol("accesskit_node_set_value", bound.node_set_value) &&
        bind_symbol("accesskit_node_value", bound.node_value) &&
        bind_symbol("accesskit_node_set_description", bound.node_set_description) &&
        bind_symbol("accesskit_node_description", bound.node_description) &&
        bind_symbol("accesskit_node_set_keyboard_shortcut", bound.node_set_keyboard_shortcut) &&
        bind_symbol("accesskit_node_keyboard_shortcut", bound.node_keyboard_shortcut) &&
        bind_symbol("accesskit_node_set_bounds", bound.node_set_bounds) &&
        bind_symbol("accesskit_node_bounds", bound.node_bounds) &&
        bind_symbol("accesskit_node_set_numeric_value", bound.node_set_numeric_value) &&
        bind_symbol("accesskit_node_numeric_value", bound.node_numeric_value) &&
        bind_symbol("accesskit_node_set_min_numeric_value", bound.node_set_min_numeric_value) &&
        bind_symbol("accesskit_node_min_numeric_value", bound.node_min_numeric_value) &&
        bind_symbol("accesskit_node_set_max_numeric_value", bound.node_set_max_numeric_value) &&
        bind_symbol("accesskit_node_max_numeric_value", bound.node_max_numeric_value) &&
        bind_symbol("accesskit_node_set_numeric_value_step", bound.node_set_numeric_value_step) &&
        bind_symbol("accesskit_node_numeric_value_step", bound.node_numeric_value_step) &&
        bind_symbol("accesskit_node_set_toggled", bound.node_set_toggled) &&
        bind_symbol("accesskit_node_toggled", bound.node_toggled) &&
        bind_symbol("accesskit_node_set_selected", bound.node_set_selected) &&
        bind_symbol("accesskit_node_is_selected", bound.node_is_selected) &&
        bind_symbol("accesskit_node_set_expanded", bound.node_set_expanded) &&
        bind_symbol("accesskit_node_is_expanded", bound.node_is_expanded) &&
        bind_symbol("accesskit_node_set_disabled", bound.node_set_disabled) &&
        bind_symbol("accesskit_node_is_disabled", bound.node_is_disabled) &&
        bind_symbol("accesskit_node_set_position_in_set", bound.node_set_position_in_set) &&
        bind_symbol("accesskit_node_position_in_set", bound.node_position_in_set) &&
        bind_symbol("accesskit_node_set_size_of_set", bound.node_set_size_of_set) &&
        bind_symbol("accesskit_node_size_of_set", bound.node_size_of_set) &&
        bind_symbol("accesskit_node_set_live", bound.node_set_live) &&
        bind_symbol("accesskit_node_live", bound.node_live) &&
        bind_symbol("accesskit_node_set_modal", bound.node_set_modal) &&
        bind_symbol("accesskit_node_is_modal", bound.node_is_modal) &&
        bind_symbol("accesskit_node_add_action", bound.node_add_action) &&
        bind_symbol("accesskit_node_supports_action", bound.node_supports_action) &&
        bind_symbol("accesskit_node_children", bound.node_children) &&
        bind_symbol("accesskit_windows_subclassing_adapter_new", windows.subclass_new) &&
        bind_symbol("accesskit_windows_subclassing_adapter_free", windows.subclass_free) &&
        bind_symbol(
            "accesskit_windows_subclassing_adapter_update_if_active", windows.subclass_update
        ) &&
        bind_symbol("accesskit_windows_queued_events_raise", windows.raise_events) &&
        bind_symbol("accesskit_windows_adapter_new", windows.plain_new) &&
        bind_symbol("accesskit_windows_adapter_free", windows.plain_free) &&
        bind_symbol("accesskit_windows_adapter_update_if_active", windows.plain_update) &&
        bind_symbol("accesskit_windows_adapter_update_window_focus_state", windows.plain_focus) &&
        bind_symbol("accesskit_windows_adapter_handle_wm_getobject", windows.handle_wm_getobject);
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

/// Reports whether a platform adapter is attached.
///
/// @return true while one is
bool adapter_held() {
#if defined(_WIN32)
    return windows.subclass != nullptr || windows.plain != nullptr;
#elif defined(ACCESSKIT_MACOS)
    return mac.adapter != nullptr;
#elif defined(ACCESSKIT_IOS)
    return ios.adapter != nullptr;
#elif defined(__linux__) || defined(__DragonFly__) || defined(__FreeBSD__) ||                      \
    defined(__NetBSD__) || defined(__OpenBSD__)
    return unix_adapter.adapter != nullptr;
#else
    return false;
#endif
}

/// Gives the Unix adapter the window's bounds. Other platforms take bounds
/// from the window they subclass.
///
/// @param width the window's width, in points
/// @param height the window's height, in points
void apply_bounds(int width, int height) {
#if defined(__linux__) || defined(__DragonFly__) || defined(__FreeBSD__) || defined(__NetBSD__) || \
    defined(__OpenBSD__)
    if (unix_adapter.adapter == nullptr || unix_adapter.set_bounds == nullptr)
        return;
    const accesskit_rect bounds{0, 0, static_cast<double>(width), static_cast<double>(height)};
    unix_adapter.set_bounds(unix_adapter.adapter, bounds, bounds);
#else
    (void)width;
    (void)height;
#endif
}

/// Tells the adapter whether the window holds the input focus.
///
/// @param focused true when it does
void apply_focus(bool focused) {
#if defined(_WIN32)
    if (windows.plain == nullptr || windows.plain_focus == nullptr)
        return;
    accesskit_windows_queued_events* events = windows.plain_focus(windows.plain, focused);
    if (events != nullptr && windows.raise_events != nullptr)
        windows.raise_events(events);
#elif defined(ACCESSKIT_MACOS)
    if (mac.adapter == nullptr || mac.update_focus == nullptr)
        return;
    accesskit_macos_queued_events* events = mac.update_focus(mac.adapter, focused);
    if (events != nullptr && mac.raise_events != nullptr)
        mac.raise_events(events);
#elif defined(__linux__) || defined(__DragonFly__) || defined(__FreeBSD__) ||                      \
    defined(__NetBSD__) || defined(__OpenBSD__)
    if (unix_adapter.adapter == nullptr || unix_adapter.update_focus == nullptr)
        return;
    unix_adapter.update_focus(unix_adapter.adapter, focused);
#else
    (void)focused;
#endif
}

/// Sends bounds and focus when either has changed.
///
/// @param window the game's window
void note_geometry(SDL_Window* window) {
    if (window == nullptr || !adapter_held())
        return;
    int width = 0;
    int height = 0;
    SDL_GetWindowSize(window, &width, &height);
    const int focused = (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0 ? 1 : 0;
    if (width != shown_width || height != shown_height) {
        apply_bounds(width, height);
        shown_width = width;
        shown_height = height;
    }
    if (focused != shown_focus) {
        apply_focus(focused == 1);
        shown_focus = focused;
    }
}

/// Asks the adapter for an update. The factory runs only when a reader is
/// connected, and it must not return null, so the caller checks that it ran.
///
/// @return true when the factory built a tree
bool push_active_update() {
    clear_snapshot_factory();
    if (update_hook != nullptr)
        return update_hook();
#if defined(_WIN32)
    accesskit_windows_queued_events* events = nullptr;
    if (windows.subclass != nullptr && windows.subclass_update != nullptr)
        events = windows.subclass_update(windows.subclass, update_from_snapshot, nullptr);
    else if (windows.plain != nullptr && windows.plain_update != nullptr)
        events = windows.plain_update(windows.plain, update_from_snapshot, nullptr);
    else
        return false;
    if (events != nullptr && windows.raise_events != nullptr)
        windows.raise_events(events);
#elif defined(ACCESSKIT_MACOS)
    if (mac.adapter == nullptr || mac.update_if_active == nullptr)
        return false;
    accesskit_macos_queued_events* events =
        mac.update_if_active(mac.adapter, update_from_snapshot, nullptr);
    if (events != nullptr && mac.raise_events != nullptr)
        mac.raise_events(events);
#elif defined(ACCESSKIT_IOS)
    if (ios.adapter == nullptr || ios.update_if_active == nullptr)
        return false;
    accesskit_ios_queued_events* events =
        ios.update_if_active(ios.adapter, update_from_snapshot, nullptr);
    if (events != nullptr && ios.raise_events != nullptr)
        ios.raise_events(events);
#elif defined(__linux__) || defined(__DragonFly__) || defined(__FreeBSD__) ||                      \
    defined(__NetBSD__) || defined(__OpenBSD__)
    if (unix_adapter.adapter == nullptr || unix_adapter.update_if_active == nullptr)
        return false;
    unix_adapter.update_if_active(unix_adapter.adapter, update_from_snapshot, nullptr);
#else
    return false;
#endif
    return snapshot_factory_ran();
}

/// Logs one line the first time it is asked.
///
/// @param[in,out] logged set once the line has been written
/// @param line the line, without a newline
void log_once(bool& logged, const char* line) {
    if (logged)
        return;
    logged = true;
    std::cerr << line << '\n';
}

bool attach(SDL_Window* window) {
    if (window == nullptr || load() != LoadResult::ready)
        return false;
    if (attached_window == window && adapter_held())
        return true;
    detach();
#if defined(_WIN32)
    const SDL_PropertiesID properties = SDL_GetWindowProperties(window);
    auto* window_handle = static_cast<HWND>(
        SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr)
    );
    if (window_handle == nullptr)
        return false;
    // The subclassing adapter aborts when the window is already visible, so
    // it is used only while the window is still hidden. A window that is
    // already showing takes the plain adapter, which needs the real focus
    // and a subclass of our own for the reader's messages.
    if ((SDL_GetWindowFlags(window) & SDL_WINDOW_HIDDEN) != 0) {
        if (windows.subclass_new == nullptr)
            return false;
        windows.subclass =
            windows.subclass_new(window_handle, on_activate, nullptr, on_action, nullptr);
        if (windows.subclass == nullptr)
            return false;
    } else {
        if (windows.plain_new == nullptr || windows.handle_wm_getobject == nullptr ||
            !subclass_hooks_ready())
            return false;
        const bool focused = (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
        windows.plain = windows.plain_new(window_handle, focused, on_action, nullptr);
        if (windows.plain == nullptr)
            return false;
        if (!install_plain_subclass(window_handle)) {
            if (windows.plain_free != nullptr)
                windows.plain_free(windows.plain);
            windows.plain = nullptr;
            return false;
        }
    }
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
    mac.adapter = mac.adapter_for_window(native, on_activate, nullptr, on_action, nullptr);
    if (mac.adapter == nullptr)
        return false;
#elif defined(ACCESSKIT_IOS)
    const SDL_PropertiesID properties = SDL_GetWindowProperties(window);
    void* native =
        SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr);
    // SDL does not report the root view controller. A missing native window
    // is the refusal it allows; the caller keeps that controller in place.
    if (native == nullptr || ios.adapter_for_window == nullptr)
        return false;
    ios.adapter = ios.adapter_for_window(
        native, on_activate, nullptr, on_action, nullptr, on_deactivate, nullptr
    );
    if (ios.adapter == nullptr)
        return false;
#elif defined(__linux__) || defined(__DragonFly__) || defined(__FreeBSD__) ||                      \
    defined(__NetBSD__) || defined(__OpenBSD__)
    if (unix_adapter.adapter_new == nullptr)
        return false;
    const char* bus = SDL_getenv("DBUS_SESSION_BUS_ADDRESS");
    if (bus == nullptr || bus[0] == '\0') {
        static bool logged_bus = false;
        log_once(logged_bus, "screen reader: no session bus, so the bridge is not started");
        return false;
    }
    unix_adapter.adapter =
        unix_adapter.adapter_new(on_activate, nullptr, on_action, nullptr, on_deactivate, nullptr);
    if (unix_adapter.adapter == nullptr) {
        static bool logged_adapter = false;
        log_once(logged_adapter, "screen reader: the bridge did not start");
        return false;
    }
#else
    return false;
#endif
    attached_window = window;
    note_geometry(window);
    return true;
}

void detach() {
#if defined(_WIN32)
    remove_plain_subclass(windows.plain_window);
    if (windows.subclass != nullptr && windows.subclass_free != nullptr)
        windows.subclass_free(windows.subclass);
    windows.subclass = nullptr;
    if (windows.plain != nullptr && windows.plain_free != nullptr)
        windows.plain_free(windows.plain);
    windows.plain = nullptr;
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
    attached_window = nullptr;
    shown_width = -1;
    shown_height = -1;
    shown_focus = -1;
    reader_active.store(false, std::memory_order_release);
    forget_sent_tree();
}

void set_preference(Preference mode) {
    if (mode == chosen_preference)
        return;
    chosen_preference = mode;
    attach_gave_up = false;
    skipped_window = nullptr;
    forget_spoken_focus();
    if (mode == Preference::off) {
        detach();
        speech_needed = false;
    }
}

bool attached() {
    return adapter_held();
}

bool attached_to(SDL_Window* window) {
    return window != nullptr && attached_window == window && adapter_held();
}

bool attach_skipped(SDL_Window* window) {
    return attach_gave_up && skipped_window == window;
}

void note_attach_finished(SDL_Window* window, bool succeeded) {
    if (succeeded) {
        attach_gave_up = false;
        skipped_window = nullptr;
        speech_needed = false;
        return;
    }
    attach_gave_up = true;
    skipped_window = window;
    speech_needed = chosen_preference != Preference::off;
}

bool speech_where_bridge_cannot() {
#if !defined(_WIN32)
    return false;
#else
    if (!speech_needed || chosen_preference == Preference::off)
        return false;
    if (chosen_preference == Preference::on)
        return true;
    return reader_running();
#endif
}

bool announcements_open() {
    if (chosen_preference == Preference::off)
        return false;
    return chosen_preference == Preference::on || reader_running();
}

bool reader_running() {
    static bool have_cache = false;
    static bool cached = false;
    static Uint64 checked_at = 0;
    const Uint64 now = SDL_GetTicks();
    if (have_cache && now - checked_at < 500)
        return cached;
    cached = query_reader_running();
    checked_at = now;
    have_cache = true;
    return cached;
}

bool active() {
    return reader_active.load(std::memory_order_acquire);
}

void refresh_window(SDL_Window* window) {
    note_geometry(window);
}

/// Adds one live node for each spoken line, under the window.
///
/// Each line keeps the politeness it asked for. The node sits inside the
/// window, not at the screen's origin. An id the tree already uses is left
/// alone. A new id changes the tree, so a repeated line is raised too.
///
/// @param[in,out] nodes the controls about to be staged
void attach_live_region(std::vector<Node>& nodes) {
    const std::vector<LiveLine> lines = live_lines();
    std::size_t index = 0;
    for (const LiveLine& line : lines) {
        if (line.id == 0 || line.id == window_node || line.text.empty())
            continue;
        bool present = false;
        for (const Node& node : nodes) {
            if (node.id == line.id)
                present = true;
        }
        if (present)
            continue;
        Node region;
        region.id = line.id;
        region.role = Role::label;
        region.label = line.text;
        region.live =
            line.politeness == Politeness::assertive ? LiveSetting::assertive : LiveSetting::polite;
        region.x = 8;
        region.y = 8 + static_cast<double>(index) * 16;
        region.width = 32;
        region.height = 16;
        for (Node& node : nodes) {
            if (node.id == window_node) {
                node.children.push_back(line.id);
                break;
            }
        }
        nodes.push_back(std::move(region));
        ++index;
    }
}

void submit_tree(std::vector<Node> nodes, uint64_t focus) {
    if (!reader_active.load(std::memory_order_acquire))
        return;
    attach_live_region(nodes);
    if (!stage_tree(std::move(nodes), focus))
        return;
    const uint64_t marked = staged_fingerprint();
    if (push_active_update())
        mark_tree_sent(marked);
}

void testing_set_reader_active(bool on) {
    reader_active.store(on, std::memory_order_release);
}

void testing_set_update_hook(bool (*hook)()) {
    update_hook = hook;
}

} // namespace oa::platform::screen_reader

#else

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace oa::platform::screen_reader {
namespace {

Preference chosen_preference = Preference::automatic;

#if defined(_WIN32) && !defined(OA_WINDOWS_95)
/// Whether Narrator says it is running. Narrator does not set the
/// screen-reader flag: it keeps its state where Settings reads it, under
/// HKEY_CURRENT_USER. Where Narrator keeps no such value, the answer is
/// false.
///
/// @return true while Narrator is running
bool narrator_running() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(
            HKEY_CURRENT_USER, L"Software\\Microsoft\\Narrator\\NoRoam", 0, KEY_QUERY_VALUE, &key
        ) != ERROR_SUCCESS)
        return false;
    DWORD state = 0;
    DWORD type = 0;
    DWORD size = sizeof state;
    const LONG read = RegQueryValueExW(
        key, L"RunningState", nullptr, &type, reinterpret_cast<BYTE*>(&state), &size
    );
    RegCloseKey(key);
    return read == ERROR_SUCCESS && type == REG_DWORD && size == sizeof state && state != 0;
}
#endif

#if defined(_WIN32)
/// Asks Windows whether a screen reader is running: the screen-reader flag,
/// or, past Windows 95, Narrator's own state. The answer is not cached.
///
/// @return true while a screen reader is running
bool query_screen_reader() {
    BOOL running = FALSE;
#ifndef SPI_GETSCREENREADER
    constexpr UINT screen_reader_flag = 0x0046;
#else
    constexpr UINT screen_reader_flag = SPI_GETSCREENREADER;
#endif
#if defined(OA_WINDOWS_95)
    if (SystemParametersInfoA(screen_reader_flag, 0, &running, 0) == FALSE)
        return false;
    return running != FALSE;
#else
    if (SystemParametersInfoW(screen_reader_flag, 0, &running, 0) != FALSE && running != FALSE)
        return true;
    return narrator_running();
#endif
}
#endif

} // namespace

bool compiled_in() {
    return false;
}

LoadResult load() {
    return LoadResult::compiled_out;
}

void set_preference(Preference mode) {
    if (mode == chosen_preference)
        return;
    chosen_preference = mode;
    forget_spoken_focus();
}

bool attach(SDL_Window* /*window*/) {
    return false;
}

void detach() {
}

bool attached() {
    return false;
}

bool attached_to(SDL_Window* /*window*/) {
    return false;
}

bool attach_skipped(SDL_Window* /*window*/) {
    return true;
}

void note_attach_finished(SDL_Window* /*window*/, bool /*succeeded*/) {
}

bool speech_where_bridge_cannot() {
#if !defined(_WIN32)
    return false;
#else
    if (chosen_preference == Preference::off)
        return false;
    if (chosen_preference == Preference::on)
        return true;
    return reader_running();
#endif
}

bool announcements_open() {
    if (chosen_preference == Preference::off)
        return false;
    return chosen_preference == Preference::on || reader_running();
}

bool reader_running() {
#if defined(_WIN32)
    static bool have_cache = false;
    static bool cached = false;
    static DWORD checked_at = 0;
    const DWORD now = GetTickCount();
    if (have_cache && now - checked_at < 500)
        return cached;
    cached = query_screen_reader();
    checked_at = now;
    have_cache = true;
    return cached;
#else
    return false;
#endif
}

bool active() {
    return false;
}

void refresh_window(SDL_Window* /*window*/) {
}

} // namespace oa::platform::screen_reader

#endif
