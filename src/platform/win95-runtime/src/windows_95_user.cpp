// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The user32 functions Windows 95 does not export, defined under the names a
// program imports them by, so that a program linked with this file imports
// none of them: Windows refuses to start one that imports a function its
// system DLLs do not export. Each call looks the system's own function up and
// uses it where the running Windows has it — Windows 98, 2000 and XP have most
// of these, some of them better than a stand-in can — and stands in where it
// does not. Built only for MinGW, whose import naming it follows, and beside
// windows_functions.cpp, whose macros and system lookup this file repeats
// because they are private to it.

// The C++ standard headers come first, before the Windows 95 declarations are
// pinned below: they reach the toolchain's thread support, whose condition
// variables a win32-threaded toolchain declares for the version this file is
// compiled at, not for Windows 95.
#include <atomic>
#include <cstddef>

// The declarations of Windows 95, so that none of the functions defined here
// is also declared as the system's.
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0400
#undef WINVER
#define WINVER 0x0400
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// The symbol a function is defined by, and the import pointer a program calls
// it through, in MinGW's naming; see windows_functions.cpp, which defines the
// same four.
#if defined(__i386__)
#define OA_XP_SYSTEM_SYMBOL(name, argument_bytes) "_" #name "@" #argument_bytes
#define OA_XP_SYSTEM_IMPORT(name, argument_bytes) "__imp__" #name "@" #argument_bytes
#else
#define OA_XP_SYSTEM_SYMBOL(name, argument_bytes) #name
#define OA_XP_SYSTEM_IMPORT(name, argument_bytes) "__imp_" #name
#endif

// Defines `function`, declared before with its type, as the system function
// `name` and as the import pointer a program calls `name` through.
#define OA_XP_DEFINE_SYSTEM(function, name, argument_bytes)                                        \
    extern "C" constinit decltype(&function)                                                       \
        const function##_import __asm__(OA_XP_SYSTEM_IMPORT(name, argument_bytes)) = &function

namespace {

/// A system function looked up on first use.
///
/// The lookup gives every thread the same answer, so threads that look it up
/// at the same time agree; the result is kept in atomics, which need no code
/// to initialise them.
template <typename Function>
struct SystemFunction {
    const wchar_t* library{};
    const char* name{};
    std::atomic<Function> function{};
    std::atomic<bool> looked_up{};

    /// Returns the function, or null when the running Windows lacks it.
    ///
    /// @return the function the library exports under name
    Function get() noexcept {
        if (!looked_up.load(std::memory_order_acquire)) {
            const HMODULE module = GetModuleHandleW(library);
            Function found = nullptr;
            if (module != nullptr)
                found = reinterpret_cast<Function>(
                    reinterpret_cast<void*>(GetProcAddress(module, name))
                );
            function.store(found, std::memory_order_relaxed);
            looked_up.store(true, std::memory_order_release);
        }
        return function.load(std::memory_order_relaxed);
    }
};

constexpr wchar_t user32[] = L"user32.dll";
constexpr wchar_t comctl32[] = L"comctl32.dll";

/// The one display a Windows 95 machine can drive, and the monitor on it.
///
/// No call makes such a monitor handle, so the stand-ins hand out this word
/// where a Windows 95 machine has a single monitor and recognise it again.
constexpr ULONG_PTR single_monitor = 1;

constexpr wchar_t primary_display[] = L"\\\\.\\DISPLAY1";

/// Copies a wide string into a fixed-size array of the Windows API's fields,
/// ending it and never writing past the array.
template <std::size_t Size>
void copy_wide(wchar_t (&out)[Size], const wchar_t* text) noexcept {
    std::size_t index = 0;
    for (; index + 1 < Size && text[index] != L'\0'; ++index)
        out[index] = text[index];
    out[index] = L'\0';
}

using AllowSetForegroundFunction = BOOL(WINAPI*)(DWORD);
using ChangeDisplaySettingsFunction = LONG(WINAPI*)(LPCWSTR, LPDEVMODEW, HWND, DWORD, LPVOID);
using EnumDisplayDevicesFunction = BOOL(WINAPI*)(LPCWSTR, DWORD, PDISPLAY_DEVICEW, DWORD);
using EnumDisplayMonitorsFunction = BOOL(WINAPI*)(HDC, LPCRECT, MONITORENUMPROC, LPARAM);
using EnumDisplaySettingsFunction = BOOL(WINAPI*)(LPCWSTR, DWORD, LPDEVMODEW);
using EnumDisplaySettingsAFunction = BOOL(WINAPI*)(LPCSTR, DWORD, LPDEVMODEA);
using FlashWindowFunction = BOOL(WINAPI*)(PFLASHWINFO);
using ClipboardSequenceFunction = DWORD(WINAPI*)(void);
using MonitorInfoFunction = BOOL(WINAPI*)(HMONITOR, LPMONITORINFO);
using MonitorFromPointFunction = HMONITOR(WINAPI*)(POINT, DWORD);
using MonitorFromWindowFunction = HMONITOR(WINAPI*)(HWND, DWORD);
using DeviceNotificationFunction = HDEVNOTIFY(WINAPI*)(HANDLE, LPVOID, DWORD);
using DropDeviceNotificationFunction = BOOL(WINAPI*)(HDEVNOTIFY);
using RawInputListFunction = UINT(WINAPI*)(PRAWINPUTDEVICELIST, PUINT, UINT);
using RawInputDeviceInfoFunction = UINT(WINAPI*)(HANDLE, UINT, LPVOID, PUINT);
using RawInputDataFunction = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
using RawInputBufferFunction = UINT(WINAPI*)(PRAWINPUT, PUINT, UINT);
using RegisterRawInputFunction = BOOL(WINAPI*)(PCRAWINPUTDEVICE, UINT, UINT);
using LayeredWindowFunction = BOOL(WINAPI*)(HWND, COLORREF, BYTE, DWORD);
using TrackMouseFunction = BOOL(WINAPI*)(LPTRACKMOUSEEVENT);

constinit SystemFunction<AllowSetForegroundFunction> system_allow_set_foreground{
    user32, "AllowSetForegroundWindow"
};
constinit SystemFunction<ChangeDisplaySettingsFunction> system_change_display_settings{
    user32, "ChangeDisplaySettingsExW"
};
constinit SystemFunction<EnumDisplayDevicesFunction> system_enum_display_devices{
    user32, "EnumDisplayDevicesW"
};
constinit SystemFunction<EnumDisplayMonitorsFunction> system_enum_display_monitors{
    user32, "EnumDisplayMonitors"
};
constinit SystemFunction<EnumDisplaySettingsFunction> system_enum_display_settings{
    user32, "EnumDisplaySettingsW"
};
using RegisterClassExFunction = ATOM(WINAPI*)(const WNDCLASSEXW*);
using CreateWindowExFunction = HWND(WINAPI*)(
    DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID
);
constinit SystemFunction<RegisterClassExFunction> system_register_class_ex{
    user32, "RegisterClassExW"
};
constinit SystemFunction<CreateWindowExFunction> system_create_window_ex{user32, "CreateWindowExW"};

using SetWindowLongFunction = LONG(WINAPI*)(HWND, int, LONG);
using GetWindowLongFunction = LONG(WINAPI*)(HWND, int);
using SetWindowTextFunction = BOOL(WINAPI*)(HWND, LPCWSTR);
using GetWindowTextFunction = int(WINAPI*)(HWND, LPWSTR, int);
using GetWindowTextLengthFunction = int(WINAPI*)(HWND);
using RegisterWindowMessageFunction = UINT(WINAPI*)(LPCWSTR);
using RegisterClipboardFormatFunction = UINT(WINAPI*)(LPCWSTR);
using LoadCursorFunction = HANDLE(WINAPI*)(HINSTANCE, LPCWSTR);
using LoadIconFunction = HANDLE(WINAPI*)(HINSTANCE, LPCWSTR);
using GetClassInfoExFunction = BOOL(WINAPI*)(HINSTANCE, LPCWSTR, LPWNDCLASSEXW);
constinit SystemFunction<SetWindowLongFunction> system_set_window_long{user32, "SetWindowLongW"};
constinit SystemFunction<GetWindowLongFunction> system_get_window_long{user32, "GetWindowLongW"};
constinit SystemFunction<SetWindowTextFunction> system_set_window_text{user32, "SetWindowTextW"};
constinit SystemFunction<GetWindowTextFunction> system_get_window_text{user32, "GetWindowTextW"};
constinit SystemFunction<GetWindowTextLengthFunction> system_get_window_text_length{
    user32, "GetWindowTextLengthW"
};
constinit SystemFunction<RegisterWindowMessageFunction> system_register_window_message{
    user32, "RegisterWindowMessageW"
};
constinit SystemFunction<RegisterClipboardFormatFunction> system_register_clipboard_format{
    user32, "RegisterClipboardFormatW"
};
constinit SystemFunction<LoadCursorFunction> system_load_cursor_w{user32, "LoadCursorW"};
constinit SystemFunction<LoadIconFunction> system_load_icon_w{user32, "LoadIconW"};
constinit SystemFunction<GetClassInfoExFunction> system_get_class_info_ex{
    user32, "GetClassInfoExW"
};
using RegisterClassFunction = ATOM(WINAPI*)(const WNDCLASSW*);
using UnregisterClassFunction = BOOL(WINAPI*)(LPCWSTR, HINSTANCE);
using DefWindowProcFunction = LRESULT(WINAPI*)(HWND, UINT, WPARAM, LPARAM);
using GetMessageFunction = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT);
using PeekMessageFunction = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT, UINT);
using DispatchMessageFunction = LRESULT(WINAPI*)(const MSG*);
using PostMessageFunction = BOOL(WINAPI*)(HWND, UINT, WPARAM, LPARAM);
using SendMessageFunction = LRESULT(WINAPI*)(HWND, UINT, WPARAM, LPARAM);
constinit SystemFunction<RegisterClassFunction> system_register_class{user32, "RegisterClassW"};
constinit SystemFunction<UnregisterClassFunction> system_unregister_class{
    user32, "UnregisterClassW"
};
constinit SystemFunction<DefWindowProcFunction> system_def_window_proc{user32, "DefWindowProcW"};
constinit SystemFunction<GetMessageFunction> system_get_message{user32, "GetMessageW"};
constinit SystemFunction<PeekMessageFunction> system_peek_message{user32, "PeekMessageW"};
constinit SystemFunction<DispatchMessageFunction> system_dispatch_message{
    user32, "DispatchMessageW"
};
constinit SystemFunction<PostMessageFunction> system_post_message{user32, "PostMessageW"};
constinit SystemFunction<SendMessageFunction> system_send_message{user32, "SendMessageW"};
using SystemParametersInfoFunction = BOOL(WINAPI*)(UINT, UINT, PVOID, UINT);
using CallWindowProcFunction = LRESULT(WINAPI*)(WNDPROC, HWND, UINT, WPARAM, LPARAM);
constinit SystemFunction<SystemParametersInfoFunction> system_system_parameters_info{
    user32, "SystemParametersInfoW"
};
constinit SystemFunction<CallWindowProcFunction> system_call_window_proc{user32, "CallWindowProcW"};
using GetPropFunction = HANDLE(WINAPI*)(HWND, LPCWSTR);
using SetPropFunction = BOOL(WINAPI*)(HWND, LPCWSTR, HANDLE);
using RemovePropFunction = HANDLE(WINAPI*)(HWND, LPCWSTR);
constinit SystemFunction<GetPropFunction> system_get_prop_w{user32, "GetPropW"};
constinit SystemFunction<SetPropFunction> system_set_prop_w{user32, "SetPropW"};
constinit SystemFunction<RemovePropFunction> system_remove_prop_w{user32, "RemovePropW"};
constinit SystemFunction<FlashWindowFunction> system_flash_window{user32, "FlashWindowEx"};
constinit SystemFunction<ClipboardSequenceFunction> system_clipboard_sequence{
    user32, "GetClipboardSequenceNumber"
};
constinit SystemFunction<MonitorInfoFunction> system_monitor_info{user32, "GetMonitorInfoW"};
constinit SystemFunction<RawInputBufferFunction> system_raw_input_buffer{
    user32, "GetRawInputBuffer"
};
constinit SystemFunction<RawInputDataFunction> system_raw_input_data{user32, "GetRawInputData"};
constinit SystemFunction<RawInputDeviceInfoFunction> system_raw_input_device_info{
    user32, "GetRawInputDeviceInfoA"
};
constinit SystemFunction<RawInputListFunction> system_raw_input_device_list{
    user32, "GetRawInputDeviceList"
};
constinit SystemFunction<MonitorFromPointFunction> system_monitor_from_point{
    user32, "MonitorFromPoint"
};
constinit SystemFunction<MonitorFromWindowFunction> system_monitor_from_window{
    user32, "MonitorFromWindow"
};
constinit SystemFunction<DeviceNotificationFunction> system_register_device_notification{
    user32, "RegisterDeviceNotificationW"
};
constinit SystemFunction<RegisterRawInputFunction> system_register_raw_input{
    user32, "RegisterRawInputDevices"
};
constinit SystemFunction<LayeredWindowFunction> system_layered_window{
    user32, "SetLayeredWindowAttributes"
};
constinit SystemFunction<TrackMouseFunction> system_track_mouse{user32, "TrackMouseEvent"};
constinit SystemFunction<DropDeviceNotificationFunction> system_unregister_device_notification{
    user32, "UnregisterDeviceNotification"
};
// Windows 95 and NT 4 keep TrackMouseEvent in the common controls library,
// under a name with a leading underscore, and a program built for Windows 95
// has no other reason to have loaded that library, so the lookup loads it.
std::atomic<TrackMouseFunction> common_controls_track_mouse{};
std::atomic<bool> common_controls_looked_up{};

/// Finds Windows 95's own TrackMouseEvent.
///
/// The library is loaded for the lookup and stays loaded, as one the system
/// has loaded itself would.
///
/// @return the common controls library's TrackMouseEvent, or null when it
/// cannot be had
TrackMouseFunction find_common_controls_track_mouse() noexcept {
    if (!common_controls_looked_up.load(std::memory_order_acquire)) {
        TrackMouseFunction found = nullptr;
        if (const HMODULE library = LoadLibraryW(comctl32)) {
            found = reinterpret_cast<TrackMouseFunction>(
                reinterpret_cast<void*>(GetProcAddress(library, "_TrackMouseEvent"))
            );
        }
        common_controls_track_mouse.store(found, std::memory_order_relaxed);
        common_controls_looked_up.store(true, std::memory_order_release);
    }
    return common_controls_track_mouse.load(std::memory_order_relaxed);
}

/// The display's rectangle in its own pixels.
RECT screen_rectangle() noexcept {
    RECT rectangle{};
    rectangle.right = GetSystemMetrics(SM_CXSCREEN);
    rectangle.bottom = GetSystemMetrics(SM_CYSCREEN);
    return rectangle;
}

/// Whether a point lies on the display.
bool on_screen(POINT point) noexcept {
    const RECT screen = screen_rectangle();
    return point.x >= screen.left && point.x < screen.right && point.y >= screen.top &&
           point.y < screen.bottom;
}

/// The monitor the Windows API's default flags name for a place that is off
/// every display: the primary one unless the caller asked for none.
HMONITOR fall_back_monitor(DWORD flags) noexcept {
    if (flags == MONITOR_DEFAULTTONULL)
        return nullptr;
    return reinterpret_cast<HMONITOR>(single_monitor);
}

} // namespace

/// Lets another process bring its window forward.
///
/// Windows 95 has no such permission to grant: a process it starts may come
/// forward without asking, so the stand-in reports that it may.
extern "C" BOOL WINAPI allow_set_foreground_window(DWORD process_id) __asm__(
    OA_XP_SYSTEM_SYMBOL(AllowSetForegroundWindow, 4)
);

BOOL WINAPI allow_set_foreground_window(DWORD process_id) {
    if (const auto system = system_allow_set_foreground.get())
        return system(process_id);
    return TRUE;
}

OA_XP_DEFINE_SYSTEM(allow_set_foreground_window, AllowSetForegroundWindow, 4);

/// Changes the display's mode.
///
/// Windows 95 has one display and the older call, which takes the mode's
/// numbers without naming the display: the stand-in passes the four fields a
/// mode is set from on, and any other display name fails as an unknown mode.
extern "C" LONG WINAPI change_display_settings_ex(
    LPCWSTR device_name, LPDEVMODEW mode, HWND window, DWORD flags, LPVOID parameter
) __asm__(OA_XP_SYSTEM_SYMBOL(ChangeDisplaySettingsExW, 20));

LONG WINAPI change_display_settings_ex(
    LPCWSTR device_name, LPDEVMODEW mode, HWND window, DWORD flags, LPVOID parameter
) {
    if (const auto system = system_change_display_settings.get())
        return system(device_name, mode, window, flags, parameter);
    (void)window;
    (void)parameter;
    if (device_name != nullptr && device_name[0] != L'\0' &&
        lstrcmpiW(device_name, primary_display) != 0)
        return DISP_CHANGE_BADMODE;
    if (mode == nullptr)
        return ChangeDisplaySettingsA(nullptr, flags);
    DEVMODEA wanted{};
    wanted.dmSize = sizeof(wanted);
    wanted.dmFields = mode->dmFields & (DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT |
                                        DM_DISPLAYFREQUENCY | DM_DISPLAYFLAGS);
    if ((wanted.dmFields & DM_BITSPERPEL) != 0)
        wanted.dmBitsPerPel = mode->dmBitsPerPel;
    if ((wanted.dmFields & DM_PELSWIDTH) != 0)
        wanted.dmPelsWidth = mode->dmPelsWidth;
    if ((wanted.dmFields & DM_PELSHEIGHT) != 0)
        wanted.dmPelsHeight = mode->dmPelsHeight;
    if ((wanted.dmFields & DM_DISPLAYFREQUENCY) != 0)
        wanted.dmDisplayFrequency = mode->dmDisplayFrequency;
    if ((wanted.dmFields & DM_DISPLAYFLAGS) != 0)
        wanted.dmDisplayFlags = mode->dmDisplayFlags;
    return ChangeDisplaySettingsA(&wanted, flags);
}

OA_XP_DEFINE_SYSTEM(change_display_settings_ex, ChangeDisplaySettingsExW, 20);

/// Describes a display device.
///
/// Windows 95 drives one display, and enumeration names it. Asking about a
/// display by name asks about that one display's monitors, which 95 does not
/// report, so only the display itself is described.
extern "C" BOOL WINAPI enum_display_devices(
    LPCWSTR device, DWORD number, PDISPLAY_DEVICEW information, DWORD flags
) __asm__(OA_XP_SYSTEM_SYMBOL(EnumDisplayDevicesW, 16));

BOOL WINAPI
enum_display_devices(LPCWSTR device, DWORD number, PDISPLAY_DEVICEW information, DWORD flags) {
    if (const auto system = system_enum_display_devices.get())
        return system(device, number, information, flags);
    if (information == nullptr || number != 0 || (device != nullptr && device[0] != L'\0'))
        return FALSE;
    information->cb = sizeof(*information);
    copy_wide(information->DeviceName, primary_display);
    copy_wide(information->DeviceString, L"Primary Display");
    information->StateFlags = DISPLAY_DEVICE_ATTACHED_TO_DESKTOP | DISPLAY_DEVICE_PRIMARY_DEVICE |
                              DISPLAY_DEVICE_VGA_COMPATIBLE;
    information->DeviceID[0] = L'\0';
    information->DeviceKey[0] = L'\0';
    return TRUE;
}

OA_XP_DEFINE_SYSTEM(enum_display_devices, EnumDisplayDevicesW, 16);

/// Calls back once for the monitor a Windows 95 machine has.
///
/// The handle handed to the callback is the one the other stand-ins here
/// recognise, so it can be passed to them as a Windows monitor handle is.
extern "C" BOOL WINAPI enum_display_monitors(
    HDC device_context, LPCRECT clip, MONITORENUMPROC callback, LPARAM data
) __asm__(OA_XP_SYSTEM_SYMBOL(EnumDisplayMonitors, 16));

BOOL WINAPI
enum_display_monitors(HDC device_context, LPCRECT clip, MONITORENUMPROC callback, LPARAM data) {
    if (const auto system = system_enum_display_monitors.get())
        return system(device_context, clip, callback, data);
    (void)clip;
    if (callback == nullptr)
        return FALSE;
    RECT screen = screen_rectangle();
    HDC monitor_context = device_context;
    bool made_context = false;
    if (monitor_context == nullptr) {
        monitor_context = GetDC(nullptr);
        made_context = true;
    }
    callback(reinterpret_cast<HMONITOR>(single_monitor), monitor_context, &screen, data);
    if (made_context)
        ReleaseDC(nullptr, monitor_context);
    return TRUE;
}

OA_XP_DEFINE_SYSTEM(enum_display_monitors, EnumDisplayMonitors, 16);

namespace {

/// The most modes a machine's listed modes are searched through.
constexpr DWORD most_listed_display_modes = 64;

/// Whether this names the one display a Windows 95 machine has, or none.
///
/// The numbers the mode in use is corrected from are the screen's, and a
/// Windows 95 machine has one screen; a machine with more than one answers
/// each display correctly already, so nothing of its own is corrected and a
/// display that is not the primary is left exactly as the system described it.
///
/// @param device the display a call names, or null for the desktop's own
/// @return true when that is the primary display, or none was named
bool names_primary_display(LPCSTR device) noexcept {
    if (device == nullptr || device[0] == '\0')
        return true;
    return lstrcmpiA(device, "\\\\.\\DISPLAY1") == 0;
}

/// The system's own narrow call, or null where the library is not loaded.
///
/// The lookups the rest of this file makes go through the wide
/// `GetModuleHandleW`, which is one of the calls this Windows stubs: it answers
/// nothing here, so every stand-in of this file takes its own path and the
/// system is never reached. This one is asked for with the narrow call, which
/// does answer, because the mode in use can only be corrected by reading what
/// the system itself says first.
///
/// @return the system's EnumDisplaySettingsA, or null
EnumDisplaySettingsAFunction system_enum_display_settings_a() noexcept {
    const HMODULE module = GetModuleHandleA("user32.dll");
    if (module == nullptr)
        return nullptr;
    return reinterpret_cast<EnumDisplaySettingsAFunction>(
        reinterpret_cast<void*>(GetProcAddress(module, "EnumDisplaySettingsA"))
    );
}

/// The numbers of the mode the screen is in, asked of the calls that describe
/// the screen rather than of the display driver.
///
/// These are the calls that answer correctly on a Windows 95 whose driver
/// cannot say which mode it is in, which is what makes them the ones to
/// believe; see `correct_mode_in_use`.
///
/// @param[out] width receives the screen's width in pixels
/// @param[out] height receives the screen's height in pixels
/// @param[out] bits receives the screen's depth, or 0 where it is not known
/// @return true when the screen described itself
bool screen_mode_numbers(DWORD& width, DWORD& height, DWORD& bits) noexcept {
    const int screen_width = GetSystemMetrics(SM_CXSCREEN);
    const int screen_height = GetSystemMetrics(SM_CYSCREEN);
    if (screen_width <= 0 || screen_height <= 0)
        return false;
    const HDC screen = GetDC(nullptr);
    if (screen == nullptr)
        return false;
    const int depth = GetDeviceCaps(screen, BITSPIXEL);
    ReleaseDC(nullptr, screen);
    width = static_cast<DWORD>(screen_width);
    height = static_cast<DWORD>(screen_height);
    bits = depth > 0 ? static_cast<DWORD>(depth) : 0;
    return true;
}

/// The mode the driver lists that has these numbers, if it lists one.
///
/// The description a machine gives of a mode it is not in is sound where the
/// one it gives of the mode it is in is not, so agreeing with a listed mode is
/// what makes the screen's numbers the driver's own answer rather than a
/// stand-in's opinion, and the listed mode carries the fields the screen
/// cannot say.
///
/// @param system the system's own narrow call, to list the modes with
/// @param width the width to match
/// @param height the height to match
/// @param bits the depth to match, or 0 to match any
/// @param[out] found receives the mode that matched
/// @return true when one matched
bool listed_mode_matching(
    EnumDisplaySettingsAFunction system, DWORD width, DWORD height, DWORD bits, DEVMODEA& found
) noexcept {
    for (DWORD index = 0; index < most_listed_display_modes; ++index) {
        DEVMODEA candidate{};
        candidate.dmSize = static_cast<WORD>(sizeof(candidate));
        if (!system(nullptr, index, &candidate))
            return false;
        if (candidate.dmPelsWidth == width && candidate.dmPelsHeight == height &&
            (bits == 0 || candidate.dmBitsPerPel == bits)) {
            found = candidate;
            return true;
        }
    }
    return false;
}

/// Replaces the mode in use when the machine cannot describe it.
///
/// Measured on a Windows 95 whose display driver cannot name the mode it is
/// in: the call answered TRUE for the mode in use with a width, a height and a
/// depth that matched no mode the machine has — 16044 by 18765 at 19216 bits —
/// while every mode it listed was sound and one of them, 800 by 600 at 16
/// bits, agreed with the screen. A program that centres its window on such a
/// width puts it thousands of pixels off the screen, where it never appears.
/// The two answers that agree are therefore taken, and only when they do: a
/// machine whose driver describes itself correctly is left alone.
///
/// @param[in,out] mode the mode in use, corrected in place
void correct_mode_in_use(DEVMODEA& mode) noexcept {
    DWORD width = 0;
    DWORD height = 0;
    DWORD bits = 0;
    if (!screen_mode_numbers(width, height, bits))
        return;
    if (mode.dmPelsWidth == width && mode.dmPelsHeight == height)
        return;
    const EnumDisplaySettingsAFunction system = system_enum_display_settings_a();
    if (system == nullptr)
        return;
    DEVMODEA listed{};
    if (!listed_mode_matching(system, width, height, bits, listed))
        return;
    mode.dmPelsWidth = listed.dmPelsWidth;
    mode.dmPelsHeight = listed.dmPelsHeight;
    mode.dmBitsPerPel = listed.dmBitsPerPel;
    mode.dmDisplayFlags = listed.dmDisplayFlags;
    mode.dmDisplayFrequency = listed.dmDisplayFrequency;
    mode.dmFields = listed.dmFields;
}

} // namespace

/// Describes one of the display's modes.
///
/// Windows 95 has the narrow call and answers it, but its display driver may
/// not be able to say which mode the display is in: measured on one, the call
/// answered TRUE for the mode in use with numbers that match no mode the
/// machine has. A mode in use is what a program centres its window on, so the
/// answer for that mode is checked against the modes the machine lists and the
/// numbers the screen reports, and corrected where those two agree and it does
/// not.
///
/// @param device the display to describe, or null for the desktop's own
/// @param index the mode's number, or ENUM_CURRENT_SETTINGS for the one in use
/// @param[out] mode receives the mode, filled from nothing first
/// @return TRUE when that mode was described
/// @see https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-enumdisplaysettingsa
extern "C" BOOL WINAPI enum_display_settings_a(LPCSTR device, DWORD index, LPDEVMODEA mode) __asm__(
    OA_XP_SYSTEM_SYMBOL(EnumDisplaySettingsA, 12)
);

BOOL WINAPI enum_display_settings_a(LPCSTR device, DWORD index, LPDEVMODEA mode) {
    const EnumDisplaySettingsAFunction system = system_enum_display_settings_a();
    if (system == nullptr) {
        SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
        return FALSE;
    }
    const BOOL answered = system(device, index, mode);
    if (answered && mode != nullptr && index == ENUM_CURRENT_SETTINGS &&
        names_primary_display(device))
        correct_mode_in_use(*mode);
    return answered;
}

OA_XP_DEFINE_SYSTEM(enum_display_settings_a, EnumDisplaySettingsA, 12);

/// Describes one of the modes the display can be set to.
///
/// Windows 95 answers this for the narrow call and for the wide one answers
/// nothing at all, and a program that lists a display's modes through the wide
/// name is told there is no display to list modes for: SDL asks this way, so
/// without the stand-in no window can be opened on this system.
///
/// The mode is read through the narrow call and the fields a mode is chosen by
/// are carried over — the size, the depth, the rate, the flags, and the name
/// of the device, widened — into a structure the narrow call knows nothing
/// about. The display's driver-private data is not carried over: it is written
/// by the driver of the Windows the call is made on, and none of it is a mode.
///
/// @param device the display to describe, or null for the desktop's own
/// @param index the mode's number, or ENUM_CURRENT_SETTINGS for the one in use
/// @param[out] mode receives the mode, filled from nothing first
/// @return TRUE when that mode was described
/// @see https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-enumdisplaysettingsw
extern "C" BOOL WINAPI enum_display_settings(LPCWSTR device, DWORD index, LPDEVMODEW mode) __asm__(
    OA_XP_SYSTEM_SYMBOL(EnumDisplaySettingsW, 12)
);

BOOL WINAPI enum_display_settings(LPCWSTR device, DWORD index, LPDEVMODEW mode) {
    if (const auto system = system_enum_display_settings.get())
        return system(device, index, mode);
    if (mode == nullptr) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (device != nullptr && device[0] != L'\0' && lstrcmpiW(device, primary_display) != 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    DEVMODEA narrow{};
    narrow.dmSize = sizeof(narrow);
    if (!EnumDisplaySettingsA(nullptr, index, &narrow))
        return FALSE;
    const DWORD caller_size = mode->dmSize;
    ZeroMemory(mode, caller_size != 0 ? caller_size : sizeof(*mode));
    mode->dmSize = static_cast<WORD>(sizeof(DEVMODEW));
    mode->dmSpecVersion = narrow.dmSpecVersion;
    mode->dmDriverVersion = narrow.dmDriverVersion;
    mode->dmFields = narrow.dmFields;
    mode->dmPosition = narrow.dmPosition;
    mode->dmDisplayOrientation = narrow.dmDisplayOrientation;
    mode->dmDisplayFixedOutput = narrow.dmDisplayFixedOutput;
    mode->dmColor = narrow.dmColor;
    mode->dmDuplex = narrow.dmDuplex;
    mode->dmYResolution = narrow.dmYResolution;
    mode->dmTTOption = narrow.dmTTOption;
    mode->dmCollate = narrow.dmCollate;
    mode->dmLogPixels = narrow.dmLogPixels;
    mode->dmBitsPerPel = narrow.dmBitsPerPel;
    mode->dmPelsWidth = narrow.dmPelsWidth;
    mode->dmPelsHeight = narrow.dmPelsHeight;
    mode->dmDisplayFlags = narrow.dmDisplayFlags;
    mode->dmDisplayFrequency = narrow.dmDisplayFrequency;
    mode->dmICMMethod = narrow.dmICMMethod;
    mode->dmICMIntent = narrow.dmICMIntent;
    mode->dmMediaType = narrow.dmMediaType;
    mode->dmDitherType = narrow.dmDitherType;
    mode->dmPanningWidth = narrow.dmPanningWidth;
    mode->dmPanningHeight = narrow.dmPanningHeight;
    wchar_t named[CCHDEVICENAME]{};
    if (MultiByteToWideChar(
            CP_ACP, 0, reinterpret_cast<LPCCH>(narrow.dmDeviceName), -1, named, CCHDEVICENAME
        ) <= 0)
        named[0] = L'\0';
    copy_wide(mode->dmDeviceName, named[0] != L'\0' ? named : primary_display);
    return TRUE;
}

OA_XP_DEFINE_SYSTEM(enum_display_settings, EnumDisplaySettingsW, 12);

/// Widens nothing and narrows a name the system is given, for the window
/// calls below, which take their names in the character set the system uses.
///
/// Windows 95 answers the narrow window calls and neither of the wide ones
/// used here, so a program that opens its window through them — SDL does — is
/// told the window cannot be created, with no reason given, and no window ever
/// appears. The names are the only part of these calls a character set belongs
/// to, so the rest is passed through unchanged.
///
/// @param narrow receives the name
/// @param capacity bytes narrow holds
/// @param wide the name to narrow, or null
/// @return true when there was no name or it was written and fitted
bool narrow_name(char* narrow, size_t capacity, const wchar_t* wide) noexcept {
    if (wide == nullptr) {
        narrow[0] = '\0';
        return true;
    }
    return WideCharToMultiByte(
               CP_ACP, 0, wide, -1, narrow, static_cast<int>(capacity), nullptr, nullptr
           ) > 0;
}

extern "C" ATOM WINAPI
register_class_ex(const WNDCLASSEXW* type) __asm__(OA_XP_SYSTEM_SYMBOL(RegisterClassExW, 4));

/// Registers a window class, as RegisterClassExW does, through the narrow call
/// this Windows does answer, with the two names it carries narrowed.
///
/// The window procedure is registered as it stands: the message it is called
/// with is the same message either way, and what differs between them is the
/// character set of the few messages that carry text.
///
/// @param type the class to register
/// @return the class's atom, or 0 with the error set as the narrow call sets it
ATOM WINAPI register_class_ex(const WNDCLASSEXW* type) {
    if (const auto system = system_register_class_ex.get())
        return system(type);
    if (type == nullptr || type->cbSize != sizeof(WNDCLASSEXW)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    char class_name[256]{};
    char menu_name[256]{};
    if (!narrow_name(class_name, sizeof(class_name), type->lpszClassName) ||
        !narrow_name(menu_name, sizeof(menu_name), type->lpszMenuName))
        return 0;
    WNDCLASSEXA narrow{};
    narrow.cbSize = sizeof(narrow);
    narrow.style = type->style;
    narrow.lpfnWndProc = type->lpfnWndProc;
    narrow.cbClsExtra = type->cbClsExtra;
    narrow.cbWndExtra = type->cbWndExtra;
    narrow.hInstance = type->hInstance;
    narrow.hIcon = type->hIcon;
    narrow.hCursor = type->hCursor;
    narrow.hbrBackground = type->hbrBackground;
    narrow.lpszMenuName = type->lpszMenuName != nullptr ? menu_name : nullptr;
    narrow.lpszClassName = class_name;
    narrow.hIconSm = type->hIconSm;
    return RegisterClassExA(&narrow);
}

OA_XP_DEFINE_SYSTEM(register_class_ex, RegisterClassExW, 4);

extern "C" HWND WINAPI create_window_ex(
    DWORD extended_style,
    LPCWSTR class_name,
    LPCWSTR window_name,
    DWORD style,
    int x,
    int y,
    int width,
    int height,
    HWND parent,
    HMENU menu,
    HINSTANCE instance,
    LPVOID parameter
) __asm__(OA_XP_SYSTEM_SYMBOL(CreateWindowExW, 48));

/// Creates a window, as CreateWindowExW does, through the narrow call this
/// Windows does answer, for the same reason as the class above: the class it
/// asks for is the one that call registered.
///
/// @param extended_style the extended window styles
/// @param class_name the class to create it from
/// @param window_name the title it starts with
/// @param style the window styles
/// @param x the left edge, or CW_USEDEFAULT
/// @param y the top edge, or CW_USEDEFAULT
/// @param width the width, or CW_USEDEFAULT
/// @param height the height, or CW_USEDEFAULT
/// @param parent the window it belongs to, or null
/// @param menu the menu it carries, or null
/// @param instance the instance the class was registered by
/// @param parameter what CreateWindowEx passes to the window procedure
/// @return the window, or null with the error set as the narrow call sets it
HWND WINAPI create_window_ex(
    DWORD extended_style,
    LPCWSTR class_name,
    LPCWSTR window_name,
    DWORD style,
    int x,
    int y,
    int width,
    int height,
    HWND parent,
    HMENU menu,
    HINSTANCE instance,
    LPVOID parameter
) {
    if (const auto system = system_create_window_ex.get())
        return system(
            extended_style,
            class_name,
            window_name,
            style,
            x,
            y,
            width,
            height,
            parent,
            menu,
            instance,
            parameter
        );
    char narrow_class[256]{};
    char narrow_window[256]{};
    if (!narrow_name(narrow_class, sizeof(narrow_class), class_name) ||
        !narrow_name(narrow_window, sizeof(narrow_window), window_name))
        return nullptr;
    return CreateWindowExA(
        extended_style,
        narrow_class,
        narrow_window,
        style,
        x,
        y,
        width,
        height,
        parent,
        menu,
        instance,
        parameter
    );
}

OA_XP_DEFINE_SYSTEM(create_window_ex, CreateWindowExW, 48);

// The window class and the message path.
//
// Every one of these is a stub on Windows 95 and answers
// ERROR_CALL_NOT_IMPLEMENTED there, measured by calling each of them: a
// program cannot register a class, cannot pump a message and cannot hand an
// unhandled one to the default procedure, so no window it asks for is ever
// made. The narrow calls do all of that here.
//
// A window procedure registered through the narrow call is called with the
// messages that carry text in the system's own character set, which is the
// price of the narrow calls. It is the right price because the calls that
// carry text in and out -- SetWindowTextW and GetWindowTextW -- have stand-ins
// of their own that convert it, and the messages the loop itself carries have
// no text in them at all.

/// Registers a window class, as RegisterClassW does.
///
/// SDL registers the class it makes its helper window from with this call and
/// not with the extended one, which is why the helper window was reported
/// unavailable and the window the game asked for was never made.
///
/// @param type the class to register
/// @return the class's atom, or 0 with the error set as the narrow call sets it
extern "C" ATOM WINAPI
register_class(const WNDCLASSW* type) __asm__(OA_XP_SYSTEM_SYMBOL(RegisterClassW, 4));

ATOM WINAPI register_class(const WNDCLASSW* type) {
    if (const auto system = system_register_class.get())
        return system(type);
    if (type == nullptr) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    char class_name[256]{};
    char menu_name[256]{};
    if (!narrow_name(class_name, sizeof(class_name), type->lpszClassName) ||
        !narrow_name(menu_name, sizeof(menu_name), type->lpszMenuName)) {
        return 0;
    }
    WNDCLASSA narrow{};
    narrow.style = type->style;
    narrow.lpfnWndProc = type->lpfnWndProc;
    narrow.cbClsExtra = type->cbClsExtra;
    narrow.cbWndExtra = type->cbWndExtra;
    narrow.hInstance = type->hInstance;
    narrow.hIcon = type->hIcon;
    narrow.hCursor = type->hCursor;
    narrow.hbrBackground = type->hbrBackground;
    narrow.lpszMenuName = type->lpszMenuName != nullptr ? menu_name : nullptr;
    narrow.lpszClassName = class_name;
    return RegisterClassA(&narrow);
}

OA_XP_DEFINE_SYSTEM(register_class, RegisterClassW, 4);

/// Unregisters a window class, as UnregisterClassW does.
///
/// @param class_name the class to unregister
/// @param instance the instance that registered it
/// @return true when it was unregistered
extern "C" BOOL WINAPI unregister_class(LPCWSTR class_name, HINSTANCE instance) __asm__(
    OA_XP_SYSTEM_SYMBOL(UnregisterClassW, 8)
);

BOOL WINAPI unregister_class(LPCWSTR class_name, HINSTANCE instance) {
    if (const auto system = system_unregister_class.get())
        return system(class_name, instance);
    char narrow[256]{};
    if (!narrow_name(narrow, sizeof(narrow), class_name))
        return FALSE;
    return UnregisterClassA(narrow, instance);
}

OA_XP_DEFINE_SYSTEM(unregister_class, UnregisterClassW, 8);

/// Carries out the default handling of a message a window procedure did not.
///
/// @param window the window
/// @param message the message
/// @param wparam its first parameter
/// @param lparam its second parameter
/// @return what the default handling answers
extern "C" LRESULT WINAPI def_window_proc(
    HWND window, UINT message, WPARAM wparam, LPARAM lparam
) __asm__(OA_XP_SYSTEM_SYMBOL(DefWindowProcW, 16));

LRESULT WINAPI def_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (const auto system = system_def_window_proc.get())
        return system(window, message, wparam, lparam);
    return DefWindowProcA(window, message, wparam, lparam);
}

OA_XP_DEFINE_SYSTEM(def_window_proc, DefWindowProcW, 16);

/// Takes the next message from the queue.
///
/// A message carries no character data, so the narrow call answers with the
/// same structure and nothing of it needs converting.
///
/// @param[out] message receives it
/// @param window the window to take for, or null for all of them
/// @param first the first message number wanted
/// @param last the last message number wanted
/// @return true when a message was taken
extern "C" BOOL WINAPI get_message(LPMSG message, HWND window, UINT first, UINT last) __asm__(
    OA_XP_SYSTEM_SYMBOL(GetMessageW, 16)
);

BOOL WINAPI get_message(LPMSG message, HWND window, UINT first, UINT last) {
    if (const auto system = system_get_message.get())
        return system(message, window, first, last);
    return GetMessageA(message, window, first, last);
}

OA_XP_DEFINE_SYSTEM(get_message, GetMessageW, 16);

/// Looks at the queue without waiting on it.
///
/// @param[out] message receives the message, when there is one
/// @param window the window to look for, or null for all of them
/// @param first the first message number wanted
/// @param last the last message number wanted
/// @param remove how the message is taken from the queue
/// @return true when there was a message

extern "C" BOOL WINAPI peek_message(
    LPMSG message, HWND window, UINT first, UINT last, UINT remove
) __asm__(OA_XP_SYSTEM_SYMBOL(PeekMessageW, 20));

BOOL WINAPI peek_message(LPMSG message, HWND window, UINT first, UINT last, UINT remove) {
    if (const auto system = system_peek_message.get())
        return system(message, window, first, last, remove);
    return PeekMessageA(message, window, first, last, remove);
}

OA_XP_DEFINE_SYSTEM(peek_message, PeekMessageW, 20);

/// Sends a message on to the window it belongs to.
///
/// @param message the message
/// @return what the window procedure answered
extern "C" LRESULT WINAPI
dispatch_message(const MSG* message) __asm__(OA_XP_SYSTEM_SYMBOL(DispatchMessageW, 4));

LRESULT WINAPI dispatch_message(const MSG* message) {
    if (const auto system = system_dispatch_message.get())
        return system(message);
    return DispatchMessageA(message);
}

OA_XP_DEFINE_SYSTEM(dispatch_message, DispatchMessageW, 4);

/// Puts a message in a window's queue.
///
/// A posted message may not carry a pointer at all, so nothing of it belongs to
/// a character set.
///
/// @param window the window
/// @param message the message
/// @param wparam its first parameter
/// @param lparam its second parameter
/// @return true when it was posted
extern "C" BOOL WINAPI post_message(
    HWND window, UINT message, WPARAM wparam, LPARAM lparam
) __asm__(OA_XP_SYSTEM_SYMBOL(PostMessageW, 16));

BOOL WINAPI post_message(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (const auto system = system_post_message.get())
        return system(window, message, wparam, lparam);
    return PostMessageA(window, message, wparam, lparam);
}

OA_XP_DEFINE_SYSTEM(post_message, PostMessageW, 16);

/// Sends a message to a window and waits for it to be handled.
///
/// @param window the window
/// @param message the message
/// @param wparam its first parameter
/// @param lparam its second parameter
/// @return what the window procedure answered
extern "C" LRESULT WINAPI send_message(
    HWND window, UINT message, WPARAM wparam, LPARAM lparam
) __asm__(OA_XP_SYSTEM_SYMBOL(SendMessageW, 16));

LRESULT WINAPI send_message(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (const auto system = system_send_message.get())
        return system(window, message, wparam, lparam);
    return SendMessageA(window, message, wparam, lparam);
}

OA_XP_DEFINE_SYSTEM(send_message, SendMessageW, 16);

/// Reads or writes a system-wide setting.
///
/// SDL asks this for the area of the screen a window may be placed in, which is
/// where a window that names no position of its own is put -- so a stub here is
/// a window that is never made, reported as "Couldn't create window" with
/// nothing after it. The actions SDL asks about carry a rectangle or a number
/// and never text, so the value is handed on as it came.
///
/// @param action which setting
/// @param first the setting's first parameter
/// @param value the setting's second parameter
/// @param flags how the setting is to be read or written
/// @return true when the call did what was asked of it
extern "C" BOOL WINAPI system_parameters_info(
    UINT action, UINT first, void* value, UINT flags
) __asm__(OA_XP_SYSTEM_SYMBOL(SystemParametersInfoW, 16));

BOOL WINAPI system_parameters_info(UINT action, UINT first, void* value, UINT flags) {
    if (const auto system = system_system_parameters_info.get())
        return system(action, first, value, flags);
    return SystemParametersInfoA(action, first, value, flags);
}

OA_XP_DEFINE_SYSTEM(system_parameters_info, SystemParametersInfoW, 16);

/// Calls a window procedure with a message, as CallWindowProcW does.
///
/// @param procedure the procedure to call
/// @param window the window
/// @param message the message
/// @param wparam its first parameter
/// @param lparam its second parameter
/// @return what the procedure answered
extern "C" LRESULT WINAPI call_window_proc(
    WNDPROC procedure, HWND window, UINT message, WPARAM wparam, LPARAM lparam
) __asm__(OA_XP_SYSTEM_SYMBOL(CallWindowProcW, 20));

LRESULT WINAPI
call_window_proc(WNDPROC procedure, HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (const auto system = system_call_window_proc.get())
        return system(procedure, window, message, wparam, lparam);
    return CallWindowProcA(procedure, window, message, wparam, lparam);
}

OA_XP_DEFINE_SYSTEM(call_window_proc, CallWindowProcW, 20);

// A window's own properties, which SDL keeps the window it belongs to in.
//
// These are stubs on Windows 95 as well, and SDL stores the window it made
// with one of them and reads it back with another -- so a stub here is a
// window made and then lost, reported as "SetProp() failed". The name is a
// string and is narrowed; the value is the caller's and is passed through,
// since narrowing a handle would corrupt it.

/// Reads a window's property.
///
/// @param window the window
/// @param name which property
/// @return its value, or null
extern "C" HANDLE WINAPI
get_prop_w(HWND window, LPCWSTR name) __asm__(OA_XP_SYSTEM_SYMBOL(GetPropW, 8));

HANDLE WINAPI get_prop_w(HWND window, LPCWSTR name) {
    if (const auto system = system_get_prop_w.get())
        return system(window, name);
    char narrow[256]{};
    if (!narrow_name(narrow, sizeof(narrow), name))
        return nullptr;
    return GetPropA(window, narrow);
}

OA_XP_DEFINE_SYSTEM(get_prop_w, GetPropW, 8);

/// Sets a window's property.
///
/// @param window the window
/// @param name which property
/// @param value the value to store
/// @return true when it was stored
extern "C" BOOL WINAPI
set_prop_w(HWND window, LPCWSTR name, HANDLE value) __asm__(OA_XP_SYSTEM_SYMBOL(SetPropW, 12));

BOOL WINAPI set_prop_w(HWND window, LPCWSTR name, HANDLE value) {
    if (const auto system = system_set_prop_w.get())
        return system(window, name, value);
    char narrow[256]{};
    if (!narrow_name(narrow, sizeof(narrow), name))
        return FALSE;
    return SetPropA(window, narrow, value);
}

OA_XP_DEFINE_SYSTEM(set_prop_w, SetPropW, 12);

/// Removes a window's property.
///
/// @param window the window
/// @param name which property
/// @return what was stored, or null
extern "C" HANDLE WINAPI
remove_prop_w(HWND window, LPCWSTR name) __asm__(OA_XP_SYSTEM_SYMBOL(RemovePropW, 8));

HANDLE WINAPI remove_prop_w(HWND window, LPCWSTR name) {
    if (const auto system = system_remove_prop_w.get())
        return system(window, name);
    char narrow[256]{};
    if (!narrow_name(narrow, sizeof(narrow), name))
        return nullptr;
    return RemovePropA(window, narrow);
}

OA_XP_DEFINE_SYSTEM(remove_prop_w, RemovePropW, 8);

/// Flashes a window's caption or its taskbar button.
///
/// Windows 95 has the older call, which inverts the window's flash state
/// rather than naming the parts to flash: the stand-in inverts unless the
/// caller asked for the flashing to stop.
extern "C" BOOL WINAPI
flash_window_ex(PFLASHWINFO information) __asm__(OA_XP_SYSTEM_SYMBOL(FlashWindowEx, 4));

BOOL WINAPI flash_window_ex(PFLASHWINFO information) {
    if (const auto system = system_flash_window.get())
        return system(information);
    if (information == nullptr)
        return FALSE;
    return FlashWindow(information->hwnd, (information->dwFlags & FLASHW_STOP) == 0);
}

OA_XP_DEFINE_SYSTEM(flash_window_ex, FlashWindowEx, 4);

/// Returns a number that changes when the clipboard does.
///
/// Windows 95 does not count the clipboard's changes and tells no program
/// when it changes, so the stand-in reports no change at all.
extern "C" DWORD WINAPI
get_clipboard_sequence_number(void) __asm__(OA_XP_SYSTEM_SYMBOL(GetClipboardSequenceNumber, 0));

DWORD WINAPI get_clipboard_sequence_number(void) {
    if (const auto system = system_clipboard_sequence.get())
        return system();
    return 0;
}

OA_XP_DEFINE_SYSTEM(get_clipboard_sequence_number, GetClipboardSequenceNumber, 0);

/// Fills in what Windows 95 knows of the display its monitor handle names.
///
/// There is one display, so every handle the stand-ins here hand out names
/// it: its rectangle, the desktop's work area as Windows reserves it, and the
/// display name a monitor handle is asked about by.
extern "C" BOOL WINAPI get_monitor_info(HMONITOR monitor, LPMONITORINFO information) __asm__(
    OA_XP_SYSTEM_SYMBOL(GetMonitorInfoW, 8)
);

BOOL WINAPI get_monitor_info(HMONITOR monitor, LPMONITORINFO information) {
    if (const auto system = system_monitor_info.get())
        return system(monitor, information);
    if (monitor == nullptr || information == nullptr || information->cbSize < sizeof(MONITORINFO)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    information->rcMonitor = screen_rectangle();
    information->rcWork = information->rcMonitor;
    RECT work{};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0))
        information->rcWork = work;
    information->dwFlags = MONITORINFOF_PRIMARY;
    if (information->cbSize >= sizeof(MONITORINFOEXW))
        copy_wide(reinterpret_cast<LPMONITORINFOEXW>(information)->szDevice, primary_display);
    return TRUE;
}

OA_XP_DEFINE_SYSTEM(get_monitor_info, GetMonitorInfoW, 8);

/// Reads raw input, which Windows 95 does not have.
///
/// Nothing on Windows 95 delivers raw input; a program that asks for it gets
/// the failure the call gives when no such input is there, and reads its
/// input from the window messages it is sent instead.
extern "C" UINT WINAPI get_raw_input_buffer(
    PRAWINPUT buffer, PUINT count, UINT header_size
) __asm__(OA_XP_SYSTEM_SYMBOL(GetRawInputBuffer, 12));

UINT WINAPI get_raw_input_buffer(PRAWINPUT buffer, PUINT count, UINT header_size) {
    if (const auto system = system_raw_input_buffer.get())
        return system(buffer, count, header_size);
    (void)buffer;
    (void)count;
    (void)header_size;
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return static_cast<UINT>(-1);
}

OA_XP_DEFINE_SYSTEM(get_raw_input_buffer, GetRawInputBuffer, 12);

/// Reads one raw input message, which Windows 95 does not have.
extern "C" UINT WINAPI get_raw_input_data(
    HRAWINPUT input, UINT command, LPVOID data, PUINT size, UINT header_size
) __asm__(OA_XP_SYSTEM_SYMBOL(GetRawInputData, 20));

UINT WINAPI
get_raw_input_data(HRAWINPUT input, UINT command, LPVOID data, PUINT size, UINT header_size) {
    if (const auto system = system_raw_input_data.get())
        return system(input, command, data, size, header_size);
    (void)input;
    (void)command;
    (void)data;
    (void)size;
    (void)header_size;
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return static_cast<UINT>(-1);
}

OA_XP_DEFINE_SYSTEM(get_raw_input_data, GetRawInputData, 20);

/// Describes a raw input device, which Windows 95 does not have.
extern "C" UINT WINAPI get_raw_input_device_info(
    HANDLE device, UINT command, LPVOID data, PUINT size
) __asm__(OA_XP_SYSTEM_SYMBOL(GetRawInputDeviceInfoA, 16));

UINT WINAPI get_raw_input_device_info(HANDLE device, UINT command, LPVOID data, PUINT size) {
    if (const auto system = system_raw_input_device_info.get())
        return system(device, command, data, size);
    (void)device;
    (void)command;
    (void)data;
    (void)size;
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return static_cast<UINT>(-1);
}

OA_XP_DEFINE_SYSTEM(get_raw_input_device_info, GetRawInputDeviceInfoA, 16);

/// Lists the raw input devices, of which Windows 95 has none.
extern "C" UINT WINAPI get_raw_input_device_list(
    PRAWINPUTDEVICELIST devices, PUINT count, UINT size
) __asm__(OA_XP_SYSTEM_SYMBOL(GetRawInputDeviceList, 12));

UINT WINAPI get_raw_input_device_list(PRAWINPUTDEVICELIST devices, PUINT count, UINT size) {
    if (const auto system = system_raw_input_device_list.get())
        return system(devices, count, size);
    (void)devices;
    (void)count;
    (void)size;
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return static_cast<UINT>(-1);
}

OA_XP_DEFINE_SYSTEM(get_raw_input_device_list, GetRawInputDeviceList, 12);

/// Names the monitor a point lies on.
///
/// Windows 95 has one monitor, so a point on the display names it and a point
/// off the display names it too unless the caller asked for none.
extern "C" HMONITOR WINAPI
monitor_from_point(POINT point, DWORD flags) __asm__(OA_XP_SYSTEM_SYMBOL(MonitorFromPoint, 12));

HMONITOR WINAPI monitor_from_point(POINT point, DWORD flags) {
    if (const auto system = system_monitor_from_point.get())
        return system(point, flags);
    if (on_screen(point))
        return reinterpret_cast<HMONITOR>(single_monitor);
    return fall_back_monitor(flags);
}

OA_XP_DEFINE_SYSTEM(monitor_from_point, MonitorFromPoint, 12);

/// Names the monitor a window is on.
///
/// Windows 95 has one monitor, so a window names it, and a window there is
/// none of is treated as a point off the display is.
extern "C" HMONITOR WINAPI
monitor_from_window(HWND window, DWORD flags) __asm__(OA_XP_SYSTEM_SYMBOL(MonitorFromWindow, 8));

HMONITOR WINAPI monitor_from_window(HWND window, DWORD flags) {
    if (const auto system = system_monitor_from_window.get())
        return system(window, flags);
    if (window == nullptr && flags == MONITOR_DEFAULTTONULL)
        return nullptr;
    return reinterpret_cast<HMONITOR>(single_monitor);
}

OA_XP_DEFINE_SYSTEM(monitor_from_window, MonitorFromWindow, 8);

/// Asks to be told of a device's comings and goings, which Windows 95 does
/// not report.
extern "C" HDEVNOTIFY WINAPI register_device_notification(
    HANDLE recipient, LPVOID filter, DWORD flags
) __asm__(OA_XP_SYSTEM_SYMBOL(RegisterDeviceNotificationW, 12));

HDEVNOTIFY WINAPI register_device_notification(HANDLE recipient, LPVOID filter, DWORD flags) {
    if (const auto system = system_register_device_notification.get())
        return system(recipient, filter, flags);
    (void)recipient;
    (void)filter;
    (void)flags;
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return nullptr;
}

OA_XP_DEFINE_SYSTEM(register_device_notification, RegisterDeviceNotificationW, 12);

/// Asks for raw input, which Windows 95 does not have.
extern "C" BOOL WINAPI register_raw_input_devices(
    PCRAWINPUTDEVICE devices, UINT count, UINT size
) __asm__(OA_XP_SYSTEM_SYMBOL(RegisterRawInputDevices, 12));

BOOL WINAPI register_raw_input_devices(PCRAWINPUTDEVICE devices, UINT count, UINT size) {
    if (const auto system = system_register_raw_input.get())
        return system(devices, count, size);
    (void)devices;
    (void)count;
    (void)size;
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

OA_XP_DEFINE_SYSTEM(register_raw_input_devices, RegisterRawInputDevices, 12);

/// Sets how much of a window shows through, which Windows 95 windows cannot.
extern "C" BOOL WINAPI set_layered_window_attributes(
    HWND window, COLORREF key, BYTE alpha, DWORD flags
) __asm__(OA_XP_SYSTEM_SYMBOL(SetLayeredWindowAttributes, 16));

BOOL WINAPI set_layered_window_attributes(HWND window, COLORREF key, BYTE alpha, DWORD flags) {
    if (const auto system = system_layered_window.get())
        return system(window, key, alpha, flags);
    (void)window;
    (void)key;
    (void)alpha;
    (void)flags;
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

OA_XP_DEFINE_SYSTEM(set_layered_window_attributes, SetLayeredWindowAttributes, 16);

/// Asks to be told when the pointer leaves or rests on a window.
///
/// Windows 95 keeps this call in its common controls library, under a name
/// with a leading underscore, where the system has no such export of its own.
extern "C" BOOL WINAPI
track_mouse_event(LPTRACKMOUSEEVENT event) __asm__(OA_XP_SYSTEM_SYMBOL(TrackMouseEvent, 4));

BOOL WINAPI track_mouse_event(LPTRACKMOUSEEVENT event) {
    if (const auto system = system_track_mouse.get())
        return system(event);
    if (const auto common_controls = find_common_controls_track_mouse())
        return common_controls(event);
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

OA_XP_DEFINE_SYSTEM(track_mouse_event, TrackMouseEvent, 4);

/// Stops being told of a device's comings and goings, which Windows 95 never
/// reported.
extern "C" BOOL WINAPI unregister_device_notification(HDEVNOTIFY notification) __asm__(
    OA_XP_SYSTEM_SYMBOL(UnregisterDeviceNotification, 4)
);

BOOL WINAPI unregister_device_notification(HDEVNOTIFY notification) {
    if (const auto system = system_unregister_device_notification.get())
        return system(notification);
    (void)notification;
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

OA_XP_DEFINE_SYSTEM(unregister_device_notification, UnregisterDeviceNotification, 4);

// The window's own data and title, which SDL sets as it creates its window.
// Both wide window calls are stubs here and neither has a wide semantic: the
// value SetWindowLongW carries is not a string but a pointer the window stores,
// so it is passed through exactly as given. Narrowing it would be fatal and
// silent, which is why the pair below does nothing to it.

extern "C" LONG WINAPI set_window_long(HWND window, int index, LONG value) __asm__(
    OA_XP_SYSTEM_SYMBOL(SetWindowLongW, 12)
);

/// Sets a window's own data, as SetWindowLongW does, through the narrow call.
///
/// The value is the caller's and is passed through unchanged: the wide and
/// narrow calls differ in their name only, both storing whatever they are
/// given. A window's user data is a pointer, and narrowing a pointer would
/// corrupt it without failing.
///
/// @param window the window
/// @param index which value to set
/// @param value the value to store
/// @return what the call it stands for returns
LONG WINAPI set_window_long(HWND window, int index, LONG value) {
    if (const auto system = system_set_window_long.get())
        return system(window, index, value);
    return SetWindowLongA(window, index, value);
}

OA_XP_DEFINE_SYSTEM(set_window_long, SetWindowLongW, 12);

extern "C" LONG WINAPI
get_window_long(HWND window, int index) __asm__(OA_XP_SYSTEM_SYMBOL(GetWindowLongW, 8));

/// Reads a window's own data, as GetWindowLongW does, through the narrow call.
///
/// @param window the window
/// @param index which value to read
/// @return the value the narrow call returns
LONG WINAPI get_window_long(HWND window, int index) {
    if (const auto system = system_get_window_long.get())
        return system(window, index);
    return GetWindowLongA(window, index);
}

OA_XP_DEFINE_SYSTEM(get_window_long, GetWindowLongW, 8);

extern "C" BOOL WINAPI
set_window_text(HWND window, LPCWSTR text) __asm__(OA_XP_SYSTEM_SYMBOL(SetWindowTextW, 8));

/// Sets a window's title, as SetWindowTextW does, through the narrow call.
///
/// @param window the window
/// @param text the title, or null for none
/// @return what the narrow call returns
BOOL WINAPI set_window_text(HWND window, LPCWSTR text) {
    if (const auto system = system_set_window_text.get())
        return system(window, text);
    char narrow[512]{};
    if (!narrow_name(narrow, sizeof(narrow), text))
        return FALSE;
    return SetWindowTextA(window, text != nullptr ? narrow : nullptr);
}

OA_XP_DEFINE_SYSTEM(set_window_text, SetWindowTextW, 8);

extern "C" int WINAPI get_window_text(HWND window, LPWSTR text, int capacity) __asm__(
    OA_XP_SYSTEM_SYMBOL(GetWindowTextW, 12)
);

/// Reads a window's title, as GetWindowTextW does, through the narrow call,
/// widening what it wrote.
///
/// @param window the window
/// @param text receives the title
/// @param capacity characters text holds
/// @return the characters written, as the wide call returns them
int WINAPI get_window_text(HWND window, LPWSTR text, int capacity) {
    if (const auto system = system_get_window_text.get())
        return system(window, text, capacity);
    if (text == nullptr || capacity <= 0)
        return 0;
    char narrow[512]{};
    const int limit =
        capacity < static_cast<int>(sizeof(narrow)) ? capacity : static_cast<int>(sizeof(narrow));
    const int written = GetWindowTextA(window, narrow, limit);
    if (written <= 0) {
        text[0] = L'\0';
        return 0;
    }
    const int wide = MultiByteToWideChar(CP_ACP, 0, narrow, -1, text, capacity);
    if (wide <= 0) {
        text[0] = L'\0';
        return 0;
    }
    return wide - 1;
}

OA_XP_DEFINE_SYSTEM(get_window_text, GetWindowTextW, 12);

extern "C" int WINAPI
get_window_text_length(HWND window) __asm__(OA_XP_SYSTEM_SYMBOL(GetWindowTextLengthW, 4));

/// Reads a window's title length, as GetWindowTextLengthW does, through the
/// narrow call: the length takes no character set.
///
/// @param window the window
/// @return the length the narrow call returns
int WINAPI get_window_text_length(HWND window) {
    if (const auto system = system_get_window_text_length.get())
        return system(window);
    return GetWindowTextLengthA(window);
}

OA_XP_DEFINE_SYSTEM(get_window_text_length, GetWindowTextLengthW, 4);

extern "C" UINT WINAPI
register_window_message(LPCWSTR text) __asm__(OA_XP_SYSTEM_SYMBOL(RegisterWindowMessageW, 4));

/// Asks for a message number by name, as RegisterWindowMessageW does, through
/// the narrow call: SDL uses one to notice its taskbar button being made.
///
/// @param text the name
/// @return the message number the narrow call returns
UINT WINAPI register_window_message(LPCWSTR text) {
    if (const auto system = system_register_window_message.get())
        return system(text);
    char narrow[256]{};
    if (!narrow_name(narrow, sizeof(narrow), text))
        return 0;
    return RegisterWindowMessageA(narrow);
}

OA_XP_DEFINE_SYSTEM(register_window_message, RegisterWindowMessageW, 4);

extern "C" UINT WINAPI
register_clipboard_format(LPCWSTR text) __asm__(OA_XP_SYSTEM_SYMBOL(RegisterClipboardFormatW, 4));

/// Asks for a clipboard format by name, as RegisterClipboardFormatW does,
/// through the narrow call.
///
/// @param text the name
/// @return the format the narrow call returns
UINT WINAPI register_clipboard_format(LPCWSTR text) {
    if (const auto system = system_register_clipboard_format.get())
        return system(text);
    char narrow[256]{};
    if (!narrow_name(narrow, sizeof(narrow), text))
        return 0;
    return RegisterClipboardFormatA(narrow);
}

OA_XP_DEFINE_SYSTEM(register_clipboard_format, RegisterClipboardFormatW, 4);

/// A resource name, which a window call may carry as an integer rather than a
/// string, and which must not be narrowed when it is one.
///
/// @param name the name, or an integer made a pointer by MAKEINTRESOURCE
/// @return true when it is an integer, which travels as it stands
bool resource_is_integer(LPCWSTR name) noexcept {
    return name != nullptr && reinterpret_cast<ULONG_PTR>(name) <= 0xFFFF;
}

extern "C" HANDLE WINAPI
load_cursor_w(HINSTANCE instance, LPCWSTR name) __asm__(OA_XP_SYSTEM_SYMBOL(LoadCursorW, 8));

/// Loads a cursor, as LoadCursorW does, through the narrow call, narrowing the
/// name only when it is a name: a resource may be named by an integer instead,
/// and that integer is the same value to both calls.
///
/// @param instance the module holding it, or null for a system cursor
/// @param name which cursor
/// @return the cursor the narrow call returns
HANDLE WINAPI load_cursor_w(HINSTANCE instance, LPCWSTR name) {
    if (const auto system = system_load_cursor_w.get())
        return system(instance, name);
    if (name == nullptr || resource_is_integer(name))
        return LoadCursorA(instance, reinterpret_cast<LPCSTR>(name));
    char narrow[256]{};
    if (!narrow_name(narrow, sizeof(narrow), name))
        return nullptr;
    return LoadCursorA(instance, narrow);
}

OA_XP_DEFINE_SYSTEM(load_cursor_w, LoadCursorW, 8);

extern "C" HANDLE WINAPI
load_icon_w(HINSTANCE instance, LPCWSTR name) __asm__(OA_XP_SYSTEM_SYMBOL(LoadIconW, 8));

/// Loads an icon, as LoadIconW does, through the narrow call, for the same
/// reason as the cursor above.
///
/// @param instance the module holding it, or null for a system icon
/// @param name which icon
/// @return the icon the narrow call returns
HANDLE WINAPI load_icon_w(HINSTANCE instance, LPCWSTR name) {
    if (const auto system = system_load_icon_w.get())
        return system(instance, name);
    if (name == nullptr || resource_is_integer(name))
        return LoadIconA(instance, reinterpret_cast<LPCSTR>(name));
    char narrow[256]{};
    if (!narrow_name(narrow, sizeof(narrow), name))
        return nullptr;
    return LoadIconA(instance, narrow);
}

OA_XP_DEFINE_SYSTEM(load_icon_w, LoadIconW, 8);

extern "C" BOOL WINAPI get_class_info_ex(
    HINSTANCE instance, LPCWSTR name, LPWNDCLASSEXW type
) __asm__(OA_XP_SYSTEM_SYMBOL(GetClassInfoExW, 12));

/// Reads a registered window class, as GetClassInfoExW does, through the narrow
/// call, widening the two names it answers with.
///
/// @param instance the module the class was registered by
/// @param name the class name
/// @param type receives the class
/// @return what the narrow call returns
BOOL WINAPI get_class_info_ex(HINSTANCE instance, LPCWSTR name, LPWNDCLASSEXW type) {
    if (const auto system = system_get_class_info_ex.get())
        return system(instance, name, type);
    if (type == nullptr || type->cbSize != sizeof(WNDCLASSEXW)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    char narrow[256]{};
    if (!narrow_name(narrow, sizeof(narrow), name))
        return FALSE;
    WNDCLASSEXA found{};
    found.cbSize = sizeof(found);
    if (!GetClassInfoExA(instance, narrow, &found))
        return FALSE;
    type->style = found.style;
    type->lpfnWndProc = found.lpfnWndProc;
    type->cbClsExtra = found.cbClsExtra;
    type->cbWndExtra = found.cbWndExtra;
    type->hInstance = found.hInstance;
    type->hIcon = found.hIcon;
    type->hCursor = found.hCursor;
    type->hbrBackground = found.hbrBackground;
    type->lpszMenuName = nullptr;
    type->lpszClassName = nullptr;
    type->hIconSm = found.hIconSm;
    if (type->lpszClassName != nullptr && found.lpszClassName != nullptr)
        MultiByteToWideChar(
            CP_ACP, 0, found.lpszClassName, -1, const_cast<LPWSTR>(type->lpszClassName), 256
        );
    return TRUE;
}

OA_XP_DEFINE_SYSTEM(get_class_info_ex, GetClassInfoExW, 12);
