// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The device, shell, COM and process-status functions that Windows 95 lacks,
// defined under the names a program imports them by. Built only for MinGW, and
// linked whole into every executable of a Windows 95 build, so that the program
// imports none of them: the libraries they come from — setupapi.dll and
// psapi.dll — are not on a Windows 95 installation at all, and the wide forms
// of the shell calls are newer than it.
//
// A definition here also displaces the system's own on a later Windows, which
// links this library whole as well, so each looks the system's function up and
// calls it when the running Windows has one, and stands in only when it does
// not.


// The C++ standard headers come first, before the Windows XP declarations are
// pinned below: they reach the toolchain's thread support, whose condition
// variables a win32-threaded toolchain declares for the version this file is
// compiled at, not for Windows XP.
#include <atomic>

// The declarations of Windows XP, so that none of the functions defined here is
// also declared as the system's.
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0501
#undef WINVER
#define WINVER 0x0501
#undef PSAPI_VERSION
#define PSAPI_VERSION 1
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cfgmgr32.h>
#include <ole2.h>
#include <propidl.h>
#include <psapi.h>
#include <setupapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <winreg.h>

// The symbol a function is defined by, and the import pointer a program calls
// it through, in MinGW's naming: on 32-bit x86 a leading underscore, and for
// the system's functions the size of their arguments.
#if defined(__i386__)
#define OA_W95_SYSTEM_SYMBOL(name, argument_bytes) "_" #name "@" #argument_bytes
#define OA_W95_SYSTEM_IMPORT(name, argument_bytes) "__imp__" #name "@" #argument_bytes
#else
#define OA_W95_SYSTEM_SYMBOL(name, argument_bytes) #name
#define OA_W95_SYSTEM_IMPORT(name, argument_bytes) "__imp_" #name
#endif

// Defines `function`, declared before with its type, as the system function
// `name` and as the import pointer a program calls `name` through.
#define OA_W95_DEFINE_SYSTEM(function, name, argument_bytes)                                       \
    extern "C" constinit decltype(&function)                                                       \
        const function##_import __asm__(OA_W95_SYSTEM_IMPORT(name, argument_bytes)) = &function

namespace {

/// A system function of a library the program does not import, looked up on
/// first use and loading the library to find it.
///
/// The lookup gives every thread the same answer, so threads that look it up at
/// the same time agree; the result is kept in atomics, which need no code to
/// initialise them.
template <typename Function>
struct LibraryFunction {
    const wchar_t* library{};
    const char* name{};
    std::atomic<Function> function{};
    std::atomic<bool> looked_up{};

    /// Returns the function, or null when the running Windows lacks it.
    ///
    /// @return the function the library exports under name
    Function get() noexcept {
        if (!looked_up.load(std::memory_order_acquire)) {
            const HMODULE module = LoadLibraryW(library);
            Function found = nullptr;
            if (module != nullptr)
                found =
                    reinterpret_cast<Function>(reinterpret_cast<void*>(GetProcAddress(module, name)));
            function.store(found, std::memory_order_relaxed);
            looked_up.store(true, std::memory_order_release);
        }
        return function.load(std::memory_order_relaxed);
    }
};

constexpr wchar_t setupapi_library[] = L"setupapi.dll";
constexpr wchar_t shell32_library[] = L"shell32.dll";
constexpr wchar_t ole32_library[] = L"ole32.dll";
constexpr wchar_t psapi_library[] = L"psapi.dll";

// Configuration Manager's failure: the node or the device asked for is not
// there, which is what every device call below answers on Windows 95.
constexpr CONFIGRET config_failure = 0x13;

// CSIDL's flag asking for the folder to be made when it is not there. Windows
// 95's own folder call has no such flag, and the folder it names is one the
// shell has already made.
constexpr int csidl_flag_create = 0x8000;

constinit LibraryFunction<decltype(&SetupDiGetClassDevsA)> system_setup_di_get_class_devs{
    setupapi_library, "SetupDiGetClassDevsA"
};
constinit LibraryFunction<decltype(&SetupDiDestroyDeviceInfoList)>
    system_setup_di_destroy_device_info_list{setupapi_library, "SetupDiDestroyDeviceInfoList"};
constinit LibraryFunction<decltype(&SetupDiEnumDeviceInfo)> system_setup_di_enum_device_info{
    setupapi_library, "SetupDiEnumDeviceInfo"
};
constinit LibraryFunction<decltype(&SetupDiGetDeviceInstanceIdA)>
    system_setup_di_get_device_instance_id{setupapi_library, "SetupDiGetDeviceInstanceIdA"};
constinit LibraryFunction<decltype(&SetupDiGetDeviceRegistryPropertyW)>
    system_setup_di_get_device_registry_property_w{
        setupapi_library, "SetupDiGetDeviceRegistryPropertyW"
    };
constinit LibraryFunction<decltype(&CM_Locate_DevNodeA)> system_cm_locate_dev_node{
    setupapi_library, "CM_Locate_DevNodeA"
};
constinit LibraryFunction<decltype(&CM_Get_Parent)> system_cm_get_parent{
    setupapi_library, "CM_Get_Parent"
};
constinit LibraryFunction<decltype(&CM_Get_Device_IDA)> system_cm_get_device_id{
    setupapi_library, "CM_Get_Device_IDA"
};

constinit LibraryFunction<decltype(&SHBrowseForFolderW)> system_sh_browse_for_folder{
    shell32_library, "SHBrowseForFolderW"
};
constinit LibraryFunction<decltype(&SHGetFolderPathW)> system_sh_get_folder_path{
    shell32_library, "SHGetFolderPathW"
};
constinit LibraryFunction<decltype(&SHGetPathFromIDListW)> system_sh_get_path_from_id_list{
    shell32_library, "SHGetPathFromIDListW"
};
constinit LibraryFunction<decltype(&Shell_NotifyIconW)> system_shell_notify_icon{
    shell32_library, "Shell_NotifyIconW"
};

constinit LibraryFunction<decltype(&CoInitializeEx)> system_co_initialize_ex{
    ole32_library, "CoInitializeEx"
};
constinit LibraryFunction<decltype(&PropVariantClear)> system_prop_variant_clear{
    ole32_library, "PropVariantClear"
};

constinit LibraryFunction<decltype(&EnumProcessModules)> system_enum_process_modules{
    psapi_library, "EnumProcessModules"
};
constinit LibraryFunction<decltype(&GetProcessMemoryInfo)> system_get_process_memory_info{
    psapi_library, "GetProcessMemoryInfo"
};

/// Copies a system-character-set string into a wide one.
///
/// @param[out] wide receives the string
/// @param capacity wide characters the buffer holds
/// @param narrow the string to copy
/// @return true when the string fitted
bool widen_into(wchar_t* wide, int capacity, const char* narrow) noexcept {
    if (wide == nullptr || narrow == nullptr)
        return false;
    return MultiByteToWideChar(CP_ACP, 0, narrow, -1, wide, capacity) > 0;
}

/// Reads a folder's path, in the system's own character set, from the
/// identifier list the shell gives for it.
///
/// @param list the folder's identifier list
/// @param[out] characters receives the path, at most MAX_PATH characters long
/// @return true when a path was read
bool path_from_id_list(const ITEMIDLIST* list, char* characters) noexcept {
    if (list == nullptr || characters == nullptr)
        return false;
    return SHGetPathFromIDListA(list, characters) == TRUE;
}

/// A folder the shell keeps the name of in the registry, and the value it
/// keeps that name in.
struct ShellFolderName {
    int identifier{};    ///< the folder's CSIDL
    const char* value{}; ///< the registry value holding its path
};

/// The folders the shell's registry names, and what it names them.
constexpr ShellFolderName shell_folder_names[] = {
    {CSIDL_APPDATA, "AppData"},
    {CSIDL_PERSONAL, "Personal"},
    {CSIDL_WINDOWS, "Windows"},
    {CSIDL_PROGRAMS, "Programs"},
    {CSIDL_DESKTOPDIRECTORY, "Desktop"},
    {CSIDL_STARTMENU, "Start Menu"},
    {CSIDL_FONTS, "Fonts"},
    {CSIDL_TEMPLATES, "Templates"},
    {CSIDL_FAVORITES, "Favorites"},
    {CSIDL_RECENT, "Recent"},
    {CSIDL_SENDTO, "SendTo"},
};

/// The value a folder's path is kept in.
///
/// @param identifier the folder's CSIDL
/// @return the value's name, or null when the registry keeps none for it
const char* shell_folder_value(int identifier) noexcept {
    for (const ShellFolderName& entry : shell_folder_names) {
        if (entry.identifier == identifier)
            return entry.value;
    }
    return nullptr;
}

/// Reads a folder's path from the registry, which is where the shell keeps the
/// ones it names.
///
/// Windows 95's shell refuses to name a folder at all, by the call the one
/// below goes through or by any other, so the place that shell does keep the
/// names in answers here instead. The path is given whether or not the folder
/// is there, which is what the call this stands for promises a caller.
///
/// @param identifier the folder's CSIDL
/// @param[out] path receives the path, at most MAX_PATH characters long
/// @return true when the registry named the folder
bool path_from_shell_folders(int identifier, wchar_t* path) noexcept {
    const char* const value = shell_folder_value(identifier);
    if (value == nullptr || path == nullptr)
        return false;

    // The registry is read in the system's own character set. Windows 95's
    // Unicode entry points are largely stubs that fail rather than narrow ones
    // that convert, which is why the shell call above names nothing here.
    HKEY folders = nullptr;
    if (RegOpenKeyExA(
            HKEY_CURRENT_USER,
            "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders",
            0,
            KEY_QUERY_VALUE,
            &folders
        ) != ERROR_SUCCESS) {
        return false;
    }

    char characters[MAX_PATH]{};
    DWORD kind = 0;
    DWORD bytes = sizeof(characters);
    const LONG read = RegQueryValueExA(
        folders, value, nullptr, &kind, reinterpret_cast<LPBYTE>(characters), &bytes
    );
    RegCloseKey(folders);
    if (read != ERROR_SUCCESS || kind != REG_SZ || characters[0] == '\0')
        return false;
    return widen_into(path, MAX_PATH, characters);
}

/// Reads the path a folder has on a Windows that does not name it.
///
/// The shell of Windows 95 keeps no name for some folders at all — the user's
/// Application Data and its own directory among them — because it means them
/// by convention rather than by record. They are answered from the convention
/// here, and a folder neither the shell nor the registry nor this names is
/// left to fail, rather than guessed at.
///
/// The folder is not made: the call this stands for gives a path whether or
/// not the folder is there, and the program asking for it makes it.
///
/// @param identifier the folder's CSIDL
/// @param[out] path receives the path, at most MAX_PATH characters long
/// @return true when the release means the folder by convention
bool path_by_convention(int identifier, wchar_t* path) noexcept {
    // The folders Windows 95 has no record for and means by convention. This
    // list has grown one hard-won entry at a time: the folders the shell will
    // not name and the registry does not hold are answered here or nowhere,
    // and a caller that gets E_FAIL for one of them is as stuck as if the
    // call were missing.
    if (path == nullptr)
        return false;
    if (identifier != CSIDL_APPDATA && identifier != CSIDL_WINDOWS &&
        identifier != CSIDL_PERSONAL && identifier != CSIDL_PROFILE &&
        identifier != CSIDL_COMMON_APPDATA)
        return false;

    // Where Windows is, which the environment does not always agree with and
    // which the folders below hang from. Read in the system's own character
    // set, and widened: Windows 95's Unicode entry points are stubs.
    char answer[MAX_PATH]{};
    const UINT written = GetWindowsDirectoryA(answer, MAX_PATH);
    if (written == 0 || written >= MAX_PATH)
        return false;
    wchar_t windows[MAX_PATH]{};
    if (!widen_into(windows, MAX_PATH, answer))
        return false;

    // The name means the Windows directory itself, or something beside it:
    // Application Data under it, or the user's own folders, which Windows 95
    // keeps at the root of the drive Windows is on — My Documents among them.
    if (identifier == CSIDL_WINDOWS) {
        for (UINT index = 0; index <= written; ++index)
            path[index] = windows[index];
        return true;
    }

    const wchar_t* tail = L"\\Application Data";
    wchar_t root[MAX_PATH]{};
    UINT offset = 0;
    if (identifier == CSIDL_PERSONAL || identifier == CSIDL_PROFILE) {
        tail = identifier == CSIDL_PERSONAL ? L"\\My Documents" : L"";
        const std::size_t length = wcslen(windows);
        const std::size_t slash = length >= 2 && windows[1] == L':' ? 2 : 0;
        for (std::size_t index = 0; index < slash; ++index)
            root[index] = windows[index];
        offset = static_cast<UINT>(slash);
    } else {
        for (UINT index = 0; index < written; ++index)
            root[index] = windows[index];
        offset = written;
    }
    if (identifier == CSIDL_PROFILE) {
        for (UINT index = 0; index <= offset; ++index)
            path[index] = root[index];
        return true;
    }
    constexpr UINT tail_limit = MAX_PATH;
    const UINT tail_length = static_cast<UINT>(wcslen(tail));
    if (offset + tail_length >= tail_limit)
        return false;
    for (UINT index = 0; index < offset; ++index)
        path[index] = root[index];
    for (UINT index = 0; index <= tail_length; ++index)
        path[offset + index] = tail[index];
    return true;
}

} // namespace

// Device enumeration: the Configuration Manager's calls, and the SetupAPI built
// on them. Neither library is on a Windows 95 installation, and its own device
// enumeration is not reachable by these calls, so a program that asks finds
// nothing rather than being refused.

/// Finds the device information for every device of a class.
extern "C" HDEVINFO WINAPI setup_di_get_class_devs_a(
    const GUID* class_guid, PCSTR enumerator, HWND parent, DWORD flags
) __asm__(OA_W95_SYSTEM_SYMBOL(SetupDiGetClassDevsA, 16));

HDEVINFO WINAPI setup_di_get_class_devs_a(
    const GUID* class_guid, PCSTR enumerator, HWND parent, DWORD flags
) {
    if (const auto system = system_setup_di_get_class_devs.get())
        return system(class_guid, enumerator, parent, flags);
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return INVALID_HANDLE_VALUE;
}

OA_W95_DEFINE_SYSTEM(setup_di_get_class_devs_a, SetupDiGetClassDevsA, 16);

/// Releases the device information a class enumerator made.
extern "C" BOOL WINAPI setup_di_destroy_device_info_list(HDEVINFO info) __asm__(
    OA_W95_SYSTEM_SYMBOL(SetupDiDestroyDeviceInfoList, 4)
);

BOOL WINAPI setup_di_destroy_device_info_list(HDEVINFO info) {
    if (const auto system = system_setup_di_destroy_device_info_list.get())
        return system(info);
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

OA_W95_DEFINE_SYSTEM(setup_di_destroy_device_info_list, SetupDiDestroyDeviceInfoList, 4);

/// Takes the device information of one device of a class.
extern "C" BOOL WINAPI setup_di_enum_device_info(
    HDEVINFO info, DWORD index, PSP_DEVINFO_DATA device
) __asm__(OA_W95_SYSTEM_SYMBOL(SetupDiEnumDeviceInfo, 12));

BOOL WINAPI setup_di_enum_device_info(HDEVINFO info, DWORD index, PSP_DEVINFO_DATA device) {
    if (const auto system = system_setup_di_enum_device_info.get())
        return system(info, index, device);
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

OA_W95_DEFINE_SYSTEM(setup_di_enum_device_info, SetupDiEnumDeviceInfo, 12);

/// Reads a device's instance identifier.
extern "C" BOOL WINAPI setup_di_get_device_instance_id_a(
    HDEVINFO info, PSP_DEVINFO_DATA device, PSTR identifier, DWORD characters, PDWORD needed
) __asm__(OA_W95_SYSTEM_SYMBOL(SetupDiGetDeviceInstanceIdA, 20));

BOOL WINAPI setup_di_get_device_instance_id_a(
    HDEVINFO info, PSP_DEVINFO_DATA device, PSTR identifier, DWORD characters, PDWORD needed
) {
    if (const auto system = system_setup_di_get_device_instance_id.get())
        return system(info, device, identifier, characters, needed);
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

OA_W95_DEFINE_SYSTEM(setup_di_get_device_instance_id_a, SetupDiGetDeviceInstanceIdA, 20);

/// Reads one of a device's registry properties.
extern "C" BOOL WINAPI setup_di_get_device_registry_property_w(
    HDEVINFO info, PSP_DEVINFO_DATA device, DWORD property, PDWORD property_type, PBYTE buffer,
    DWORD bytes, PDWORD needed
) __asm__(OA_W95_SYSTEM_SYMBOL(SetupDiGetDeviceRegistryPropertyW, 28));

BOOL WINAPI setup_di_get_device_registry_property_w(
    HDEVINFO info, PSP_DEVINFO_DATA device, DWORD property, PDWORD property_type, PBYTE buffer,
    DWORD bytes, PDWORD needed
) {
    if (const auto system = system_setup_di_get_device_registry_property_w.get())
        return system(info, device, property, property_type, buffer, bytes, needed);
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

OA_W95_DEFINE_SYSTEM(setup_di_get_device_registry_property_w, SetupDiGetDeviceRegistryPropertyW, 28);

/// Finds a device node by its identifier.
extern "C" CONFIGRET WINAPI cm_locate_dev_node_a(
    PDEVINST node, DEVINSTID_A identifier, ULONG flags
) __asm__(OA_W95_SYSTEM_SYMBOL(CM_Locate_DevNodeA, 12));

CONFIGRET WINAPI cm_locate_dev_node_a(PDEVINST node, DEVINSTID_A identifier, ULONG flags) {
    if (const auto system = system_cm_locate_dev_node.get())
        return system(node, identifier, flags);
    return config_failure;
}

OA_W95_DEFINE_SYSTEM(cm_locate_dev_node_a, CM_Locate_DevNodeA, 12);

/// Takes the node above a device node in the tree.
extern "C" CONFIGRET WINAPI
cm_get_parent(PDEVINST parent, DEVINST node, ULONG flags) __asm__(
    OA_W95_SYSTEM_SYMBOL(CM_Get_Parent, 12)
);

CONFIGRET WINAPI cm_get_parent(PDEVINST parent, DEVINST node, ULONG flags) {
    if (const auto system = system_cm_get_parent.get())
        return system(parent, node, flags);
    return config_failure;
}

OA_W95_DEFINE_SYSTEM(cm_get_parent, CM_Get_Parent, 12);

/// Reads a device node's identifier.
extern "C" CONFIGRET WINAPI cm_get_device_id_a(
    DEVINST node, PSTR identifier, ULONG length, ULONG flags
) __asm__(OA_W95_SYSTEM_SYMBOL(CM_Get_Device_IDA, 16));

CONFIGRET WINAPI cm_get_device_id_a(DEVINST node, PSTR identifier, ULONG length, ULONG flags) {
    if (const auto system = system_cm_get_device_id.get())
        return system(node, identifier, length, flags);
    return config_failure;
}

OA_W95_DEFINE_SYSTEM(cm_get_device_id_a, CM_Get_Device_IDA, 16);

// The shell: the wide forms of the folder calls, answered through the narrow
// ones Windows 95's shell does have, and the tray, which is not.

/// Asks the user for a folder.
extern "C" LPITEMIDLIST WINAPI
sh_browse_for_folder_w(LPBROWSEINFOW info) __asm__(
    OA_W95_SYSTEM_SYMBOL(SHBrowseForFolderW, 4)
);

LPITEMIDLIST WINAPI sh_browse_for_folder_w(LPBROWSEINFOW info) {
    if (const auto system = system_sh_browse_for_folder.get())
        return system(info);
    if (info == nullptr) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }

    // The narrow call's structure holds the same fields, its title and the name
    // it displays being in the system's character set.
    BROWSEINFOA older{};
    older.hwndOwner = info->hwndOwner;
    older.pidlRoot = info->pidlRoot;
    older.ulFlags = info->ulFlags;
    older.lpfn = info->lpfn;
    older.lParam = info->lParam;
    older.iImage = info->iImage;

    char displayed[MAX_PATH]{};
    older.pszDisplayName = displayed;

    char* title = nullptr;
    if (info->lpszTitle != nullptr) {
        const int bytes =
            WideCharToMultiByte(CP_ACP, 0, info->lpszTitle, -1, nullptr, 0, nullptr, nullptr);
        if (bytes > 0) {
            title = static_cast<char*>(CoTaskMemAlloc(static_cast<SIZE_T>(bytes)));
            if (title != nullptr)
                WideCharToMultiByte(CP_ACP, 0, info->lpszTitle, -1, title, bytes, nullptr, nullptr);
        }
    }
    older.lpszTitle = title;

    const LPITEMIDLIST chosen = SHBrowseForFolderA(&older);
    if (chosen != nullptr && info->pszDisplayName != nullptr)
        widen_into(info->pszDisplayName, MAX_PATH, displayed);
    CoTaskMemFree(title);
    return chosen;
}

OA_W95_DEFINE_SYSTEM(sh_browse_for_folder_w, SHBrowseForFolderW, 4);

/// Reads a folder's path.
extern "C" HRESULT WINAPI sh_get_folder_path_w(
    HWND owner, int folder, HANDLE token, DWORD flags, LPWSTR path
) __asm__(OA_W95_SYSTEM_SYMBOL(SHGetFolderPathW, 20));

HRESULT WINAPI
sh_get_folder_path_w(HWND owner, int folder, HANDLE token, DWORD flags, LPWSTR path) {
    if (const auto system = system_sh_get_folder_path.get())
        return system(owner, folder, token, flags, path);
    if (path == nullptr)
        return E_INVALIDARG;

    // Windows 95's shell has no Local AppData; Windows 98 and IE4 added it.
    // The user's own Application Data is where a program's files go there,
    // and is the folder the two names mean on a Windows that has both.
    int identifier = folder & ~csidl_flag_create;
    if (identifier == CSIDL_LOCAL_APPDATA)
        identifier = CSIDL_APPDATA;

    // Windows 95's shell names a folder by an identifier list, from which the
    // path is read the same way the call below reads one.
    LPITEMIDLIST list = nullptr;
    char characters[MAX_PATH]{};
    if (SUCCEEDED(SHGetSpecialFolderLocation(owner, identifier, &list))) {
        const bool read = path_from_id_list(list, characters);
        CoTaskMemFree(list);
        if (read && widen_into(path, MAX_PATH, characters))
            return S_OK;
    }

    // That shell refuses to name a folder at all here, so the registry it keeps
    // the same names in is read instead, and the folder may be one that is not
    // there yet: this call gives a path whether or not it is.
    if (path_from_shell_folders(identifier, path))
        return S_OK;

    // The folders that shell keeps no name for are the ones it means by
    // convention, which is where the last of this is read from.
    if (path_by_convention(identifier, path))
        return S_OK;
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return E_FAIL;
}

OA_W95_DEFINE_SYSTEM(sh_get_folder_path_w, SHGetFolderPathW, 20);

/// Reads a path from an identifier list.
extern "C" BOOL WINAPI
sh_get_path_from_id_list_w(PCIDLIST_ABSOLUTE list, LPWSTR path) __asm__(
    OA_W95_SYSTEM_SYMBOL(SHGetPathFromIDListW, 8)
);

BOOL WINAPI sh_get_path_from_id_list_w(PCIDLIST_ABSOLUTE list, LPWSTR path) {
    if (const auto system = system_sh_get_path_from_id_list.get())
        return system(list, path);
    if (path == nullptr) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    char characters[MAX_PATH]{};
    if (!path_from_id_list(list, characters))
        return FALSE;
    return widen_into(path, MAX_PATH, characters) ? TRUE : FALSE;
}

OA_W95_DEFINE_SYSTEM(sh_get_path_from_id_list_w, SHGetPathFromIDListW, 8);

/// Adds, changes or removes a program's icon in the taskbar's notification area.
///
/// Windows 95's shell has this call in its narrow form alone, whose structure
/// leaves the tooltip, the balloon and a program's menu strings fewer characters
/// than the wide one carries, so it is not answered through it: a program that
/// asks is told the icon could not be put there.
extern "C" BOOL WINAPI
shell_notify_icon_w(DWORD message, PNOTIFYICONDATAW icon) __asm__(
    OA_W95_SYSTEM_SYMBOL(Shell_NotifyIconW, 8)
);

BOOL WINAPI shell_notify_icon_w(DWORD message, PNOTIFYICONDATAW icon) {
    if (const auto system = system_shell_notify_icon.get())
        return system(message, icon);
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

OA_W95_DEFINE_SYSTEM(shell_notify_icon_w, Shell_NotifyIconW, 8);

// COM: the apartment call the newer one asks for, and the clearing of a typed
// value. Windows 95 has its COM library, and the older apartment call with it.

/// Starts COM on the calling thread.
///
/// Windows 95's COM has one kind of apartment, so a program that asks for the
/// multi-threaded one is given the single-threaded one.
extern "C" HRESULT WINAPI
co_initialize_ex(LPVOID reserved, DWORD flags) __asm__(OA_W95_SYSTEM_SYMBOL(CoInitializeEx, 8));

HRESULT WINAPI co_initialize_ex(LPVOID reserved, DWORD flags) {
    if (const auto system = system_co_initialize_ex.get())
        return system(reserved, flags);
    return CoInitialize(reserved);
}

OA_W95_DEFINE_SYSTEM(co_initialize_ex, CoInitializeEx, 8);

/// Releases what a typed value holds and empties it.
///
/// The stand-in releases what the types a Windows 95 program meets carry; a
/// value of any other type is emptied without its contents being released.
extern "C" HRESULT WINAPI
prop_variant_clear(PROPVARIANT* value) __asm__(OA_W95_SYSTEM_SYMBOL(PropVariantClear, 4));

HRESULT WINAPI prop_variant_clear(PROPVARIANT* value) {
    if (const auto system = system_prop_variant_clear.get())
        return system(value);
    if (value == nullptr)
        return E_INVALIDARG;

    switch (value->vt) {
    case VT_BSTR:
        SysFreeString(value->bstrVal);
        break;
    case VT_LPSTR:
        CoTaskMemFree(value->pszVal);
        break;
    case VT_LPWSTR:
        CoTaskMemFree(value->pwszVal);
        break;
    case VT_UNKNOWN:
        if (value->punkVal != nullptr)
            value->punkVal->Release();
        break;
    case VT_DISPATCH:
        if (value->pdispVal != nullptr)
            value->pdispVal->Release();
        break;
    default:
        break;
    }
    PropVariantInit(value);
    return S_OK;
}

OA_W95_DEFINE_SYSTEM(prop_variant_clear, PropVariantClear, 4);

// The process-status library, which is not on a Windows 95 installation. Its
// module list is the one Windows 95's own toolhelp gives, and it has no
// per-process memory counters to give.

/// Lists the modules the calling process has loaded.
///
/// Windows 95's toolhelp lists a process's modules, the handle this call wants
/// among their other fields; a process other than the calling one cannot be
/// named to it, so that is the one case it refuses.
extern "C" BOOL WINAPI enum_process_modules(
    HANDLE process, HMODULE* modules, DWORD bytes, LPDWORD needed
) __asm__(OA_W95_SYSTEM_SYMBOL(EnumProcessModules, 16));

BOOL WINAPI enum_process_modules(HANDLE process, HMODULE* modules, DWORD bytes, LPDWORD needed) {
    if (const auto system = system_enum_process_modules.get())
        return system(process, modules, bytes, needed);
    if (process != GetCurrentProcess()) {
        SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
        return FALSE;
    }

    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE)
        return FALSE;

    MODULEENTRY32 entry{};
    entry.dwSize = sizeof entry;
    DWORD written = 0;
    for (BOOL more = Module32First(snapshot, &entry); more; more = Module32Next(snapshot, &entry)) {
        if (modules != nullptr && written + sizeof(HMODULE) > bytes) {
            if (needed != nullptr)
                *needed = written + sizeof(HMODULE);
            CloseHandle(snapshot);
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return FALSE;
        }
        if (modules != nullptr)
            modules[written / sizeof(HMODULE)] = entry.hModule;
        written += sizeof(HMODULE);
    }
    CloseHandle(snapshot);
    if (needed != nullptr)
        *needed = written;
    return TRUE;
}

OA_W95_DEFINE_SYSTEM(enum_process_modules, EnumProcessModules, 16);

/// Reads the memory the calling process uses.
///
/// Windows 95 keeps no per-process memory counters, so a program that asks is
/// told there are none rather than being given the machine's own.
extern "C" BOOL WINAPI get_process_memory_info(
    HANDLE process, PPROCESS_MEMORY_COUNTERS counters, DWORD bytes
) __asm__(OA_W95_SYSTEM_SYMBOL(GetProcessMemoryInfo, 12));

BOOL WINAPI get_process_memory_info(HANDLE process, PPROCESS_MEMORY_COUNTERS counters, DWORD bytes) {
    if (const auto system = system_get_process_memory_info.get())
        return system(process, counters, bytes);
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

OA_W95_DEFINE_SYSTEM(get_process_memory_info, GetProcessMemoryInfo, 12);
