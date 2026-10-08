// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The kernel32 functions that Windows 95 does not export and that the C++
// run-time library and SDL call, defined under the names a program imports
// them by. A program linked with this file calls these in place of the
// system's; each uses the system's own function when the running Windows has
// it and a stand-in when it does not. Built only for MinGW, whose import
// naming it follows.
//
// The macro and SystemFunction definitions below are oa-platform-xp-runtime's
// windows_functions.cpp's, which covers the functions Windows XP lacks. The
// two modules share no header deliberately: the functions there are ones only
// the C++ run-time library calls, and these are ones SDL calls as well, which
// is why these are objects of the program and those are a library (see
// CMakeLists.txt).


// The declarations of Windows XP, so that none of the functions defined here
// is also declared as the system's.
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0501
#undef WINVER
#define WINVER 0x0501
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cstring>
#include <cwchar>
#include <iterator>

// The symbol a function is defined by, and the import pointer a program
// calls it through, in MinGW's naming: on 32-bit x86 a leading underscore,
// and for the system's functions the size of their arguments.
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
/// The lookup gives every thread the same answer, so threads that look it
/// up at the same time agree; the result is kept in atomics, which need no
/// code to initialise them.
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
                found =
                    reinterpret_cast<Function>(reinterpret_cast<void*>(GetProcAddress(module, name)));
            function.store(found, std::memory_order_relaxed);
            looked_up.store(true, std::memory_order_release);
        }
        return function.load(std::memory_order_relaxed);
    }
};

constexpr wchar_t kernel32[] = L"kernel32.dll";

} // namespace

// The version a program asks about, and the conditions it asks with. Both
// calls are needed for one question and are kept together: the mask one makes
// is the mask the other reads.

namespace {

using AddVectoredHandlerFunction =
    PVOID(WINAPI*)(ULONG first, PVECTORED_EXCEPTION_HANDLER handler);
using RemoveVectoredHandlerFunction = ULONG(WINAPI*)(PVOID handler);
using AttachConsoleFunction = BOOL(WINAPI*)(DWORD process_id);
using CancelIoFunction = BOOL(WINAPI*)(HANDLE file);
using CopyFileExFunction = BOOL(WINAPI*)(
    LPCWSTR from, LPCWSTR to, LPPROGRESS_ROUTINE progress, LPVOID data, LPBOOL cancel, DWORD flags
);
using CreateHardLinkFunction =
    BOOL(WINAPI*)(LPCWSTR file, LPCWSTR existing, LPSECURITY_ATTRIBUTES attributes);
using FindFirstFileExFunction = HANDLE(WINAPI*)(
    LPCWSTR name, FINDEX_INFO_LEVELS level, LPVOID data, FINDEX_SEARCH_OPS search, LPVOID filter,
    DWORD flags
);
using GetFileAttributesExFunction =
    BOOL(WINAPI*)(LPCWSTR name, GET_FILEEX_INFO_LEVELS level, LPVOID data);
using GetDiskFreeSpaceExFunction = BOOL(WINAPI*)(
    LPCWSTR directory, PULARGE_INTEGER available, PULARGE_INTEGER total, PULARGE_INTEGER free
);
using GetFileSizeExFunction = BOOL(WINAPI*)(HANDLE file, PLARGE_INTEGER size);
using GetLongPathNameFunction = DWORD(WINAPI*)(LPCWSTR name, LPWSTR long_name, DWORD capacity);
using GetModuleHandleExFunction = BOOL(WINAPI*)(DWORD flags, LPCWSTR name, HMODULE* module);
using GetUserDefaultUILanguageFunction = LANGID(WINAPI*)(void);
using GlobalMemoryStatusExFunction = BOOL(WINAPI*)(LPMEMORYSTATUSEX status);
using InitializeCriticalSectionAndSpinCountFunction =
    BOOL(WINAPI*)(LPCRITICAL_SECTION section, DWORD spin_count);
using IsWow64ProcessFunction = BOOL(WINAPI*)(HANDLE process, PBOOL wow64);
using SetFilePointerExFunction =
    BOOL(WINAPI*)(HANDLE file, LARGE_INTEGER distance, PLARGE_INTEGER position, DWORD method);
using SetThreadExecutionStateFunction = EXECUTION_STATE(WINAPI*)(EXECUTION_STATE state);
using TryEnterCriticalSectionFunction = BOOL(WINAPI*)(LPCRITICAL_SECTION section);
using InitializeConditionVariableFunction = void(WINAPI*)(void** condition);
using SleepConditionVariableFunction = BOOL(WINAPI*)(void** condition, void* section, DWORD timeout_ms);
using VerSetConditionMaskFunction = ULONGLONG(WINAPI*)(ULONGLONG mask, DWORD type, BYTE condition);
using VerifyVersionInfoFunction =
    BOOL(WINAPI*)(LPOSVERSIONINFOEXW wanted, DWORD type, DWORDLONG mask);

constinit SystemFunction<AddVectoredHandlerFunction> system_add_vectored_handler{
    kernel32, "AddVectoredExceptionHandler"
};
constinit SystemFunction<RemoveVectoredHandlerFunction> system_remove_vectored_handler{
    kernel32, "RemoveVectoredExceptionHandler"
};
constinit SystemFunction<AttachConsoleFunction> system_attach_console{kernel32, "AttachConsole"};
constinit SystemFunction<CancelIoFunction> system_cancel_io{kernel32, "CancelIo"};
constinit SystemFunction<CopyFileExFunction> system_copy_file_ex{kernel32, "CopyFileExW"};
constinit SystemFunction<CreateHardLinkFunction> system_create_hard_link{
    kernel32, "CreateHardLinkW"
};
constinit SystemFunction<FindFirstFileExFunction> system_find_first_file_ex{
    kernel32, "FindFirstFileExW"
};
using WideFindFirstFunction = HANDLE(WINAPI*)(LPCWSTR, LPWIN32_FIND_DATAW);
using WideFindNextFunction = BOOL(WINAPI*)(HANDLE, LPWIN32_FIND_DATAW);
using WideGetFileAttributesFunction = DWORD(WINAPI*)(LPCWSTR);
using WideGetTempPathFunction = DWORD(WINAPI*)(DWORD, LPWSTR);
using WideGetCurrentDirectoryFunction = DWORD(WINAPI*)(DWORD, LPWSTR);
constinit SystemFunction<WideFindFirstFunction> system_wide_find_first{kernel32, "FindFirstFileW"};
constinit SystemFunction<WideFindNextFunction> system_wide_find_next{kernel32, "FindNextFileW"};
constinit SystemFunction<WideGetFileAttributesFunction> system_wide_get_file_attributes{
    kernel32, "GetFileAttributesW"
};
constinit SystemFunction<WideGetTempPathFunction> system_wide_get_temp_path{
    kernel32, "GetTempPathW"
};
constinit SystemFunction<WideGetCurrentDirectoryFunction> system_wide_get_current_directory{
    kernel32, "GetCurrentDirectoryW"
};
using WideGetFullPathNameFunction = DWORD(WINAPI*)(LPCWSTR, DWORD, LPWSTR, LPWSTR*);
using WideCreateDirectoryFunction = BOOL(WINAPI*)(LPCWSTR, LPSECURITY_ATTRIBUTES);
using WideSetCurrentDirectoryFunction = BOOL(WINAPI*)(LPCWSTR);
using WideMoveFileFunction = BOOL(WINAPI*)(LPCWSTR, LPCWSTR);
using WideDeleteFileFunction = BOOL(WINAPI*)(LPCWSTR);
using WideRemoveDirectoryFunction = BOOL(WINAPI*)(LPCWSTR);
using WideCopyFileFunction = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, BOOL);
constinit SystemFunction<WideGetFullPathNameFunction> system_wide_get_full_path_name{
    kernel32, "GetFullPathNameW"
};
constinit SystemFunction<WideCreateDirectoryFunction> system_wide_create_directory{
    kernel32, "CreateDirectoryW"
};
constinit SystemFunction<WideSetCurrentDirectoryFunction> system_wide_set_current_directory{
    kernel32, "SetCurrentDirectoryW"
};
constinit SystemFunction<WideMoveFileFunction> system_wide_move_file{kernel32, "MoveFileW"};
constinit SystemFunction<WideDeleteFileFunction> system_wide_delete_file{kernel32, "DeleteFileW"};
constinit SystemFunction<WideRemoveDirectoryFunction> system_wide_remove_directory{
    kernel32, "RemoveDirectoryW"
};
constinit SystemFunction<WideCopyFileFunction> system_wide_copy_file{kernel32, "CopyFileW"};
using WideGetEnvironmentStringsFunction = LPWSTR(WINAPI*)();
using WideFreeEnvironmentStringsFunction = BOOL(WINAPI*)(LPWSTR);
constinit SystemFunction<WideGetEnvironmentStringsFunction> system_wide_get_environment_strings{
    kernel32, "GetEnvironmentStringsW"
};
constinit SystemFunction<WideFreeEnvironmentStringsFunction> system_wide_free_environment_strings{
    kernel32, "FreeEnvironmentStringsW"
};
using OutputDebugStringFunction = void(WINAPI*)(LPCWSTR);
constinit SystemFunction<OutputDebugStringFunction> system_output_debug_string{
    kernel32, "OutputDebugStringW"
};
constinit SystemFunction<GetDiskFreeSpaceExFunction> system_get_disk_free_space_ex{
    kernel32, "GetDiskFreeSpaceExW"
};
constinit SystemFunction<GetFileAttributesExFunction> system_get_file_attributes_ex{
    kernel32, "GetFileAttributesExW"
};
constinit SystemFunction<GetFileSizeExFunction> system_get_file_size_ex{
    kernel32, "GetFileSizeEx"
};
constinit SystemFunction<GetLongPathNameFunction> system_get_long_path_name{
    kernel32, "GetLongPathNameW"
};
constinit SystemFunction<GetModuleHandleExFunction> system_get_module_handle_ex{
    kernel32, "GetModuleHandleExW"
};
constinit SystemFunction<GetUserDefaultUILanguageFunction> system_get_user_default_ui_language{
    kernel32, "GetUserDefaultUILanguage"
};
constinit SystemFunction<GlobalMemoryStatusExFunction> system_global_memory_status_ex{
    kernel32, "GlobalMemoryStatusEx"
};
constinit SystemFunction<InitializeCriticalSectionAndSpinCountFunction>
    system_initialize_critical_section_and_spin_count{
        kernel32, "InitializeCriticalSectionAndSpinCount"
    };
constinit SystemFunction<IsWow64ProcessFunction> system_is_wow64_process{
    kernel32, "IsWow64Process"
};
constinit SystemFunction<SetFilePointerExFunction> system_set_file_pointer_ex{
    kernel32, "SetFilePointerEx"
};
constinit SystemFunction<SetThreadExecutionStateFunction> system_set_thread_execution_state{
    kernel32, "SetThreadExecutionState"
};
constinit SystemFunction<TryEnterCriticalSectionFunction> system_try_enter_critical_section{
    kernel32, "TryEnterCriticalSection"
};
constinit SystemFunction<InitializeConditionVariableFunction> system_initialize_condition_variable{
    kernel32, "InitializeConditionVariable"
};
constinit SystemFunction<SleepConditionVariableFunction> system_sleep_condition_variable{
    kernel32, "SleepConditionVariableCS"
};
constinit SystemFunction<VerSetConditionMaskFunction> system_ver_set_condition_mask{
    kernel32, "VerSetConditionMask"
};
constinit SystemFunction<VerifyVersionInfoFunction> system_verify_version_info{
    kernel32, "VerifyVersionInfoW"
};

/// The version fields a mask names, in the order their bits sit in it: a
/// field's condition occupies three bits at three times its index.
constexpr DWORD version_fields[] = {
    VER_MINORVERSION,     VER_MAJORVERSION,       VER_BUILDNUMBER, VER_PLATFORMID,
    VER_SERVICEPACKMINOR, VER_SERVICEPACKMAJOR,   VER_SUITENAME,   VER_PRODUCT_TYPE,
};

/// The value one version field of a description holds.
///
/// @param version the description
/// @param field index into version_fields
/// @return the field's value
// A template: the caller's description is the wide one and the
// running Windows' is read through the narrow call, and the two hold the
// same fields.
template <typename Version>
DWORD version_field_value(const Version& version, std::size_t field) noexcept {
    switch (field) {
    case 0:
        return version.dwMinorVersion;
    case 1:
        return version.dwMajorVersion;
    case 2:
        return version.dwBuildNumber;
    case 3:
        return version.dwPlatformId;
    case 4:
        return version.wServicePackMinor;
    case 5:
        return version.wServicePackMajor;
    case 6:
        return version.wSuiteMask;
    default:
        return version.wProductType;
    }
}

/// Whether a field's value meets the condition the mask holds for it.
///
/// @param value the running Windows' value
/// @param wanted the description's value
/// @param condition the three-bit condition, one of the VER_ ones
/// @return true when the condition holds
bool version_meets_condition(DWORD value, DWORD wanted, BYTE condition) noexcept {
    switch (condition) {
    case VER_EQUAL:
        return value == wanted;
    case VER_GREATER:
        return value > wanted;
    case VER_GREATER_EQUAL:
        return value >= wanted;
    case VER_LESS:
        return value < wanted;
    case VER_LESS_EQUAL:
        return value <= wanted;
    case VER_AND:
        return (value & wanted) == wanted;
    case VER_OR:
        return (value & wanted) != 0;
    default:
        return false;
    }
}

} // namespace

/// Reports that a thread's exceptions may be seen before the system handles
/// them.
///
/// Windows 95 has no vectored handlers, so none runs; a caller installs one to
/// watch its own threads, and reporting the installation with the handler as
/// the token a later removal passes back costs nothing.
///
/// @param first true to see exceptions before the system's handling, false after
/// @param handler the handler to install
/// @return the token to remove with, which is the handler
extern "C" PVOID WINAPI add_vectored_exception_handler(
    ULONG first, PVECTORED_EXCEPTION_HANDLER handler
) __asm__(OA_XP_SYSTEM_SYMBOL(AddVectoredExceptionHandler, 8));

PVOID WINAPI
add_vectored_exception_handler(ULONG first, PVECTORED_EXCEPTION_HANDLER handler) {
    if (const auto system = system_add_vectored_handler.get())
        return system(first, handler);
    return reinterpret_cast<PVOID>(handler);
}

OA_XP_DEFINE_SYSTEM(add_vectored_exception_handler, AddVectoredExceptionHandler, 8);

/// Removes a handler add_vectored_exception_handler installed.
///
/// @param handler the token that call returned
/// @return true
extern "C" ULONG WINAPI
remove_vectored_exception_handler(PVOID handler) __asm__(
    OA_XP_SYSTEM_SYMBOL(RemoveVectoredExceptionHandler, 4)
);

ULONG WINAPI remove_vectored_exception_handler(PVOID handler) {
    if (const auto system = system_remove_vectored_handler.get())
        return system(handler);
    return TRUE;
}

OA_XP_DEFINE_SYSTEM(remove_vectored_exception_handler, RemoveVectoredExceptionHandler, 4);

/// Attaches the calling process to a console's.
///
/// Windows 95 has no console subsystem, so there is none to attach to.
///
/// @param process_id the process whose console to attach to
/// @return false, with the last error saying the call is not there
extern "C" BOOL WINAPI
attach_console(DWORD process_id) __asm__(OA_XP_SYSTEM_SYMBOL(AttachConsole, 4));

BOOL WINAPI attach_console(DWORD process_id) {
    if (const auto system = system_attach_console.get())
        return system(process_id);
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

OA_XP_DEFINE_SYSTEM(attach_console, AttachConsole, 4);

/// Cancels the input and output a file has outstanding.
///
/// Windows 95 has no asynchronous input and output to cancel.
///
/// @param file the file whose input and output to cancel
/// @return false, with the last error saying the call is not supported
extern "C" BOOL WINAPI cancel_io(HANDLE file) __asm__(OA_XP_SYSTEM_SYMBOL(CancelIo, 4));

BOOL WINAPI cancel_io(HANDLE file) {
    if (const auto system = system_cancel_io.get())
        return system(file);
    (void)file;
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

OA_XP_DEFINE_SYSTEM(cancel_io, CancelIo, 4);

/// Copies a file, with a caller that may watch its progress.
///
/// Windows 95 copies a file whole, with no progress to report, which is what a
/// caller that asked for progress gets: none, and the copy.
///
/// @param from the file to copy
/// @param to the file to write
/// @param progress the caller's progress function, never called here
/// @param data the value to pass the progress function
/// @param[out] cancel the caller's cancel flag, set false
/// @param flags COPY_FILE_* flags
/// @return true when the file was copied
extern "C" BOOL WINAPI copy_file_ex(
    LPCWSTR from, LPCWSTR to, LPPROGRESS_ROUTINE progress, LPVOID data, LPBOOL cancel, DWORD flags
) __asm__(OA_XP_SYSTEM_SYMBOL(CopyFileExW, 24));

BOOL WINAPI copy_file_ex(
    LPCWSTR from, LPCWSTR to, LPPROGRESS_ROUTINE progress, LPVOID data, LPBOOL cancel, DWORD flags
) {
    if (const auto system = system_copy_file_ex.get())
        return system(from, to, progress, data, cancel, flags);
    (void)progress;
    (void)data;
    if (cancel != nullptr)
        *cancel = FALSE;
    return CopyFileW(from, to, (flags & COPY_FILE_FAIL_IF_EXISTS) != 0);
}

OA_XP_DEFINE_SYSTEM(copy_file_ex, CopyFileExW, 24);

/// Makes a second name for a file.
///
/// Windows 95 has no hard links, on no filing system it holds.
///
/// @param file the name to make
/// @param existing the file it names
/// @param attributes ignored
/// @return false, with the last error saying the call is not supported
extern "C" BOOL WINAPI create_hard_link_w(
    LPCWSTR file, LPCWSTR existing, LPSECURITY_ATTRIBUTES attributes
) __asm__(OA_XP_SYSTEM_SYMBOL(CreateHardLinkW, 12));

BOOL WINAPI
create_hard_link_w(LPCWSTR file, LPCWSTR existing, LPSECURITY_ATTRIBUTES attributes) {
    if (const auto system = system_create_hard_link.get())
        return system(file, existing, attributes);
    (void)file;
    (void)existing;
    (void)attributes;
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

OA_XP_DEFINE_SYSTEM(create_hard_link_w, CreateHardLinkW, 12);

/// Finds the first name a pattern names.
///
/// Windows 95 searches by name alone, so the caller's level, search operation
/// and filter are not used; the name a caller searched with is the one it gets.
///
/// @param name the pattern to search with
/// @param level which fields of the find data to fill, ignored
/// @param[out] data the WIN32_FIND_DATA the search fills
/// @param search which names to search for, ignored
/// @param filter a filter for the search, ignored
/// @param flags search flags, ignored
/// @return the search handle, or INVALID_HANDLE_VALUE with the last error set
extern "C" HANDLE WINAPI find_first_file_ex(
    LPCWSTR name, FINDEX_INFO_LEVELS level, LPVOID data, FINDEX_SEARCH_OPS search, LPVOID filter,
    DWORD flags
) __asm__(OA_XP_SYSTEM_SYMBOL(FindFirstFileExW, 24));

HANDLE WINAPI find_first_file_ex(
    LPCWSTR name, FINDEX_INFO_LEVELS level, LPVOID data, FINDEX_SEARCH_OPS search, LPVOID filter,
    DWORD flags
) {
    if (const auto system = system_find_first_file_ex.get())
        return system(name, level, data, search, filter, flags);
    (void)level;
    (void)search;
    (void)filter;
    (void)flags;
    return FindFirstFileW(name, static_cast<LPWIN32_FIND_DATAW>(data));
}

OA_XP_DEFINE_SYSTEM(find_first_file_ex, FindFirstFileExW, 24);

/// Reads a file's attributes, its size and the times its entries hold.
///
/// Windows 95 answers the same search that finds a name, which holds all of
/// those for it.
///
/// @param name the file to read
/// @param level which information to read, which must be the standard one
/// @param[out] data the WIN32_FILE_ATTRIBUTE_DATA to fill
/// @return true when the file was read
extern "C" BOOL WINAPI get_file_attributes_ex(
    LPCWSTR name, GET_FILEEX_INFO_LEVELS level, LPVOID data
) __asm__(OA_XP_SYSTEM_SYMBOL(GetFileAttributesExW, 12));

BOOL WINAPI get_file_attributes_ex(LPCWSTR name, GET_FILEEX_INFO_LEVELS level, LPVOID data) {
    if (const auto system = system_get_file_attributes_ex.get())
        return system(name, level, data);
    if (level != GetFileExInfoStandard || data == nullptr) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    // Built from FindFirstFileA, which Windows 95 has, rather than from its
    // FindFirstFileW twin, which Windows 95 exports as a stub that fails — and
    // not from GetFileAttributesExA either: that name is no older, since no
    // Windows before 98 has GetFileAttributesEx in either form. What is filled
    // in here is numbers, so nothing has to be widened back.
    char narrow[MAX_PATH + 1]{};
    if (WideCharToMultiByte(CP_ACP, 0, name, -1, narrow, sizeof(narrow), nullptr, nullptr) == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    WIN32_FIND_DATAA found{};
    const HANDLE search = FindFirstFileA(narrow, &found);
    if (search == INVALID_HANDLE_VALUE)
        return FALSE;
    FindClose(search);
    auto* attributes = static_cast<LPWIN32_FILE_ATTRIBUTE_DATA>(data);
    attributes->dwFileAttributes = found.dwFileAttributes;
    attributes->ftCreationTime = found.ftCreationTime;
    attributes->ftLastAccessTime = found.ftLastAccessTime;
    attributes->ftLastWriteTime = found.ftLastWriteTime;
    attributes->nFileSizeHigh = found.nFileSizeHigh;
    attributes->nFileSizeLow = found.nFileSizeLow;
    return TRUE;
}

OA_XP_DEFINE_SYSTEM(get_file_attributes_ex, GetFileAttributesExW, 12);

/// Reads a volume's size and the room left on it.
///
/// Windows 95 RTM has only the call that answers in clusters, which the three
/// totals the caller asks for are worked out from; OSR2 and every later
/// Windows have this one.
///
/// @param directory a folder on the volume
/// @param[out] available the room the calling user may use
/// @param[out] total the volume's size
/// @param[out] free_space the room free on it
/// @return true when the volume was read
extern "C" BOOL WINAPI get_disk_free_space_ex(
    LPCWSTR directory, PULARGE_INTEGER available, PULARGE_INTEGER total, PULARGE_INTEGER free_space
) __asm__(OA_XP_SYSTEM_SYMBOL(GetDiskFreeSpaceExW, 16));

BOOL WINAPI get_disk_free_space_ex(
    LPCWSTR directory, PULARGE_INTEGER available, PULARGE_INTEGER total, PULARGE_INTEGER free_space
) {
    if (const auto system = system_get_disk_free_space_ex.get())
        return system(directory, available, total, free_space);
    char narrow[MAX_PATH + 1]{};
    if (WideCharToMultiByte(CP_ACP, 0, directory, -1, narrow, sizeof(narrow), nullptr, nullptr) == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    DWORD sectors_per_cluster = 0;
    DWORD bytes_per_sector = 0;
    DWORD free_clusters = 0;
    DWORD all_clusters = 0;
    if (GetDiskFreeSpaceA(
            narrow, &sectors_per_cluster, &bytes_per_sector, &free_clusters, &all_clusters
        ) == FALSE)
        return FALSE;
    const ULONGLONG cluster_bytes =
        static_cast<ULONGLONG>(sectors_per_cluster) * bytes_per_sector;
    if (available != nullptr)
        available->QuadPart = cluster_bytes * free_clusters;
    if (total != nullptr)
        total->QuadPart = cluster_bytes * all_clusters;
    if (free_space != nullptr)
        free_space->QuadPart = cluster_bytes * free_clusters;
    return TRUE;
}

OA_XP_DEFINE_SYSTEM(get_disk_free_space_ex, GetDiskFreeSpaceExW, 16);

// The wide file calls Windows 95 exports are stubs: measured on OSR2,
// FindFirstFileW, GetFileAttributesW and GetTempPathW all answer
// ERROR_CALL_NOT_IMPLEMENTED while their system-character-set twins succeed.
// They are answered here from those twins and the result widened, which is the
// only place this can be fixed once for everybody: mingw's std::filesystem, SDL
// and this program's own callers all reach the wide names, and no import audit
// can see a call that is present but refuses.

/// Widens a find result, whose numbers are the same in both forms.
void widen_find_data(WIN32_FIND_DATAW& wide, const WIN32_FIND_DATAA& narrow) noexcept {
    wide.dwFileAttributes = narrow.dwFileAttributes;
    wide.ftCreationTime = narrow.ftCreationTime;
    wide.ftLastAccessTime = narrow.ftLastAccessTime;
    wide.ftLastWriteTime = narrow.ftLastWriteTime;
    wide.nFileSizeHigh = narrow.nFileSizeHigh;
    wide.nFileSizeLow = narrow.nFileSizeLow;
    wide.dwReserved0 = narrow.dwReserved0;
    wide.dwReserved1 = narrow.dwReserved1;
    MultiByteToWideChar(CP_ACP, 0, narrow.cFileName, -1, wide.cFileName, MAX_PATH);
    MultiByteToWideChar(CP_ACP, 0, narrow.cAlternateFileName, -1, wide.cAlternateFileName, 14);
}

/// Finds the first name a pattern names.
extern "C" HANDLE WINAPI wide_find_first_file(
    LPCWSTR name, LPWIN32_FIND_DATAW data
) __asm__(OA_XP_SYSTEM_SYMBOL(FindFirstFileW, 8));

HANDLE WINAPI wide_find_first_file(LPCWSTR name, LPWIN32_FIND_DATAW data) {
    if (const auto system = system_wide_find_first.get())
        return system(name, data);
    if (name == nullptr || data == nullptr) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    char narrow_name[MAX_PATH + 1]{};
    if (WideCharToMultiByte(
            CP_ACP, 0, name, -1, narrow_name, sizeof(narrow_name), nullptr, nullptr
        ) == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    WIN32_FIND_DATAA narrow{};
    const HANDLE search = FindFirstFileA(narrow_name, &narrow);
    if (search == INVALID_HANDLE_VALUE)
        return search;
    widen_find_data(*data, narrow);
    return search;
}

OA_XP_DEFINE_SYSTEM(wide_find_first_file, FindFirstFileW, 8);

/// Finds the name after the one a search last answered with.
extern "C" BOOL WINAPI wide_find_next_file(
    HANDLE search, LPWIN32_FIND_DATAW data
) __asm__(OA_XP_SYSTEM_SYMBOL(FindNextFileW, 8));

BOOL WINAPI wide_find_next_file(HANDLE search, LPWIN32_FIND_DATAW data) {
    if (const auto system = system_wide_find_next.get())
        return system(search, data);
    if (data == nullptr) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    WIN32_FIND_DATAA narrow{};
    if (FindNextFileA(search, &narrow) == FALSE)
        return FALSE;
    widen_find_data(*data, narrow);
    return TRUE;
}

OA_XP_DEFINE_SYSTEM(wide_find_next_file, FindNextFileW, 8);

/// Reads a file's attributes.
extern "C" DWORD WINAPI wide_get_file_attributes(LPCWSTR name) __asm__(
    OA_XP_SYSTEM_SYMBOL(GetFileAttributesW, 4)
);

DWORD WINAPI wide_get_file_attributes(LPCWSTR name) {
    if (const auto system = system_wide_get_file_attributes.get()) {
        return system(name);
    }
    if (name == nullptr) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_FILE_ATTRIBUTES;
    }
    char narrow_name[MAX_PATH + 1]{};
    if (WideCharToMultiByte(
            CP_ACP, 0, name, -1, narrow_name, sizeof(narrow_name), nullptr, nullptr
        ) == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_FILE_ATTRIBUTES;
    }
    const DWORD answer = GetFileAttributesA(narrow_name);
    return answer;
}

OA_XP_DEFINE_SYSTEM(wide_get_file_attributes, GetFileAttributesW, 4);

/// Gives the folder temporary files go in.
extern "C" DWORD WINAPI wide_get_temp_path(DWORD length, LPWSTR path) __asm__(
    OA_XP_SYSTEM_SYMBOL(GetTempPathW, 8)
);

DWORD WINAPI wide_get_temp_path(DWORD length, LPWSTR path) {
    if (const auto system = system_wide_get_temp_path.get())
        return system(length, path);
    if (path == nullptr || length == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    char narrow[MAX_PATH + 1]{};
    const DWORD capacity = length < sizeof(narrow) ? length : sizeof(narrow);
    const DWORD written = GetTempPathA(capacity, narrow);
    if (written == 0 || written >= capacity) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    if (MultiByteToWideChar(CP_ACP, 0, narrow, -1, path, static_cast<int>(length)) == 0)
        return 0;
    return written;
}

OA_XP_DEFINE_SYSTEM(wide_get_temp_path, GetTempPathW, 8);

/// Reads the folder that is the current one.
extern "C" DWORD WINAPI wide_get_current_directory(DWORD length, LPWSTR directory) __asm__(
    OA_XP_SYSTEM_SYMBOL(GetCurrentDirectoryW, 8)
);

DWORD WINAPI wide_get_current_directory(DWORD length, LPWSTR directory) {
    if (const auto system = system_wide_get_current_directory.get())
        return system(length, directory);
    if (directory == nullptr || length == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    char narrow[MAX_PATH + 1]{};
    const DWORD capacity = length < sizeof(narrow) ? length : sizeof(narrow);
    const DWORD written = GetCurrentDirectoryA(capacity, narrow);
    if (written == 0)
        return 0;
    if (written >= capacity) {
        // The system-character-set call answers the size the buffer would
        // need, the null included, which is what the wide caller is owed when
        // its own buffer is too small.
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return written;
    }
    if (MultiByteToWideChar(CP_ACP, 0, narrow, -1, directory, static_cast<int>(length)) == 0)
        return 0;
    return written;
}

OA_XP_DEFINE_SYSTEM(wide_get_current_directory, GetCurrentDirectoryW, 8);

// The rest of the family, each measured on OSR2 as returning
// ERROR_CALL_NOT_IMPLEMENTED while its system-character-set twin succeeds.
// A path is narrowed; a result that is a path or a count is written as it comes
// back from the twin.

/// Narrows a path, answering false when it will not fit.
bool narrow_path(LPCWSTR wide, char* narrow, int capacity) noexcept {
    if (wide == nullptr || narrow == nullptr)
        return false;
    return WideCharToMultiByte(CP_ACP, 0, wide, -1, narrow, capacity, nullptr, nullptr) != 0;
}

/// Gives the full path of a name.
extern "C" DWORD WINAPI wide_get_full_path_name(
    LPCWSTR name, DWORD length, LPWSTR path, LPWSTR* file_part
) __asm__(OA_XP_SYSTEM_SYMBOL(GetFullPathNameW, 16));

DWORD WINAPI wide_get_full_path_name(LPCWSTR name, DWORD length, LPWSTR path, LPWSTR* file_part) {
    if (const auto system = system_wide_get_full_path_name.get())
        return system(name, length, path, file_part);
    char narrow_name[MAX_PATH + 1]{};
    if (!narrow_path(name, narrow_name, sizeof(narrow_name))) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (path == nullptr || length == 0) {
        // The caller is asking how much room the answer needs.
        return GetFullPathNameA(narrow_name, 0, nullptr, nullptr);
    }
    char narrow_path_out[MAX_PATH + 1]{};
    char* narrow_file_part = nullptr;
    const DWORD written =
        GetFullPathNameA(narrow_name, static_cast<DWORD>(sizeof(narrow_path_out)), narrow_path_out, &narrow_file_part);
    if (written == 0)
        return 0;
    if (written >= length) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return written;
    }
    if (MultiByteToWideChar(CP_ACP, 0, narrow_path_out, -1, path, static_cast<int>(length)) == 0)
        return 0;
    if (file_part != nullptr) {
        // The file part is where the twin's own pointer lands in the answer.
        const std::size_t offset = narrow_file_part != nullptr
                                       ? static_cast<std::size_t>(narrow_file_part - narrow_path_out)
                                       : 0;
        *file_part = path + offset;
    }
    return written;
}

OA_XP_DEFINE_SYSTEM(wide_get_full_path_name, GetFullPathNameW, 16);

/// Makes a folder.
extern "C" BOOL WINAPI wide_create_directory(
    LPCWSTR name, LPSECURITY_ATTRIBUTES attributes
) __asm__(OA_XP_SYSTEM_SYMBOL(CreateDirectoryW, 8));

BOOL WINAPI wide_create_directory(LPCWSTR name, LPSECURITY_ATTRIBUTES attributes) {
    if (const auto system = system_wide_create_directory.get())
        return system(name, attributes);
    char narrow[MAX_PATH + 1]{};
    if (!narrow_path(name, narrow, sizeof(narrow))) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    return CreateDirectoryA(narrow, attributes);
}

OA_XP_DEFINE_SYSTEM(wide_create_directory, CreateDirectoryW, 8);

/// Makes a folder the current one.
extern "C" BOOL WINAPI wide_set_current_directory(LPCWSTR name) __asm__(
    OA_XP_SYSTEM_SYMBOL(SetCurrentDirectoryW, 4)
);

BOOL WINAPI wide_set_current_directory(LPCWSTR name) {
    if (const auto system = system_wide_set_current_directory.get())
        return system(name);
    char narrow[MAX_PATH + 1]{};
    if (!narrow_path(name, narrow, sizeof(narrow))) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    return SetCurrentDirectoryA(narrow);
}

OA_XP_DEFINE_SYSTEM(wide_set_current_directory, SetCurrentDirectoryW, 4);

/// Moves a file or a folder.
extern "C" BOOL WINAPI wide_move_file(LPCWSTR from, LPCWSTR to) __asm__(
    OA_XP_SYSTEM_SYMBOL(MoveFileW, 8)
);

BOOL WINAPI wide_move_file(LPCWSTR from, LPCWSTR to) {
    if (const auto system = system_wide_move_file.get())
        return system(from, to);
    char narrow_from[MAX_PATH + 1]{};
    char narrow_to[MAX_PATH + 1]{};
    if (!narrow_path(from, narrow_from, sizeof(narrow_from)) ||
        !narrow_path(to, narrow_to, sizeof(narrow_to))) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    return MoveFileA(narrow_from, narrow_to);
}

OA_XP_DEFINE_SYSTEM(wide_move_file, MoveFileW, 8);

/// Removes a file.
extern "C" BOOL WINAPI wide_delete_file(LPCWSTR name) __asm__(
    OA_XP_SYSTEM_SYMBOL(DeleteFileW, 4)
);

BOOL WINAPI wide_delete_file(LPCWSTR name) {
    if (const auto system = system_wide_delete_file.get())
        return system(name);
    char narrow[MAX_PATH + 1]{};
    if (!narrow_path(name, narrow, sizeof(narrow))) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    return DeleteFileA(narrow);
}

OA_XP_DEFINE_SYSTEM(wide_delete_file, DeleteFileW, 4);

/// Removes a folder, which has to be empty.
extern "C" BOOL WINAPI wide_remove_directory(LPCWSTR name) __asm__(
    OA_XP_SYSTEM_SYMBOL(RemoveDirectoryW, 4)
);

BOOL WINAPI wide_remove_directory(LPCWSTR name) {
    if (const auto system = system_wide_remove_directory.get())
        return system(name);
    char narrow[MAX_PATH + 1]{};
    if (!narrow_path(name, narrow, sizeof(narrow))) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    return RemoveDirectoryA(narrow);
}

OA_XP_DEFINE_SYSTEM(wide_remove_directory, RemoveDirectoryW, 4);

/// Copies a file.
extern "C" BOOL WINAPI wide_copy_file(LPCWSTR from, LPCWSTR to, BOOL fail_if_exists) __asm__(
    OA_XP_SYSTEM_SYMBOL(CopyFileW, 12)
);

BOOL WINAPI wide_copy_file(LPCWSTR from, LPCWSTR to, BOOL fail_if_exists) {
    if (const auto system = system_wide_copy_file.get())
        return system(from, to, fail_if_exists);
    char narrow_from[MAX_PATH + 1]{};
    char narrow_to[MAX_PATH + 1]{};
    if (!narrow_path(from, narrow_from, sizeof(narrow_from)) ||
        !narrow_path(to, narrow_to, sizeof(narrow_to))) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    return CopyFileA(narrow_from, narrow_to, fail_if_exists);
}

OA_XP_DEFINE_SYSTEM(wide_copy_file, CopyFileW, 12);

/// Reads the whole environment.
///
/// SDL builds its own table of the environment from this call and answers every
/// hint out of that table, so a wide stub here tells a program that it has no
/// environment at all and hides every variable set for it. That is what kept
/// `SDL_AUDIO_DRIVER=dummy` from being seen, and left SDL choosing a sound
/// driver this machine has not got before it ever reached the video it was
/// asked for. The block is the one the narrow call answers with, widened, and
/// it is freed by the call below.
///
/// @return the block, or null when the environment cannot be read
/// @see wide_free_environment_strings

extern "C" LPWSTR WINAPI wide_get_environment_strings() __asm__(
    OA_XP_SYSTEM_SYMBOL(GetEnvironmentStringsW, 0)
);

/// The blocks this hands out, so the call that frees one can tell them from the
/// system's.
wchar_t* environment_blocks[8] = {};

LPWSTR WINAPI wide_get_environment_strings() {
    if (const auto system = system_wide_get_environment_strings.get())
        return system();

    char* narrow = GetEnvironmentStringsA();
    if (narrow == nullptr)
        return nullptr;

    // A run of null-terminated strings, ending at an empty one, and the empty
    // one that ends the copy as well.
    int characters = 1;
    for (const char* entry = narrow; *entry != '\0'; entry += std::strlen(entry) + 1) {
        characters += static_cast<int>(MultiByteToWideChar(CP_ACP, 0, entry, -1, nullptr, 0));
    }

    auto* wide = static_cast<wchar_t*>(LocalAlloc(LMEM_FIXED, characters * sizeof(wchar_t)));
    if (wide == nullptr) {
        FreeEnvironmentStringsA(narrow);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }

    wchar_t* write = wide;
    for (const char* entry = narrow; *entry != '\0'; entry += std::strlen(entry) + 1) {
        const int written = MultiByteToWideChar(
            CP_ACP, 0, entry, -1, write, characters - static_cast<int>(write - wide)
        );
        if (written == 0)
            break;
        write += written;
    }
    *write = L'\0';
    FreeEnvironmentStringsA(narrow);

    for (wchar_t*& kept : environment_blocks) {
        if (kept == nullptr) {
            kept = wide;
            break;
        }
    }
    return wide;
}

OA_XP_DEFINE_SYSTEM(wide_get_environment_strings, GetEnvironmentStringsW, 0);

/// Frees the block the call above answered with.
///
/// @param block the block
/// @return true, as the call this stands for does

extern "C" BOOL WINAPI wide_free_environment_strings(LPWSTR block) __asm__(
    OA_XP_SYSTEM_SYMBOL(FreeEnvironmentStringsW, 4)
);

BOOL WINAPI wide_free_environment_strings(LPWSTR block) {
    if (const auto system = system_wide_free_environment_strings.get())
        return system(block);
    if (block == nullptr)
        return FALSE;
    for (wchar_t*& kept : environment_blocks) {
        if (kept == block) {
            kept = nullptr;
            LocalFree(block);
            return TRUE;
        }
    }
    // Not one of ours; there is nothing of the system's here to free.
    return TRUE;
}

OA_XP_DEFINE_SYSTEM(wide_free_environment_strings, FreeEnvironmentStringsW, 4);

/// Writes a line to whatever is listening for debug output.
///
/// SDL sends every message it logs through this, so a stub here is a program
/// that says nothing at all about what it is doing or why it stopped -- which
/// is how the whole of this port came to be full of silent failures. The line
/// is narrowed and written with the narrow call.
///
/// @param text the line to write
extern "C" void WINAPI output_debug_string(LPCWSTR text) __asm__(
    OA_XP_SYSTEM_SYMBOL(OutputDebugStringW, 4)
);

void WINAPI output_debug_string(LPCWSTR text) {
    if (const auto system = system_output_debug_string.get()) {
        system(text);
        return;
    }
    char narrow[1024]{};
    if (!narrow_path(text, narrow, sizeof(narrow)))
        return;
    OutputDebugStringA(narrow);
}

OA_XP_DEFINE_SYSTEM(output_debug_string, OutputDebugStringW, 4);



/// Reads a file's size, which Windows 95 answers in two halves.
///
/// @param file the file to read
/// @param[out] size the size in bytes
/// @return true when the size was read
extern "C" BOOL WINAPI
get_file_size_ex(HANDLE file, PLARGE_INTEGER size) __asm__(
    OA_XP_SYSTEM_SYMBOL(GetFileSizeEx, 8)
);

BOOL WINAPI get_file_size_ex(HANDLE file, PLARGE_INTEGER size) {
    if (const auto system = system_get_file_size_ex.get())
        return system(file, size);
    if (size == nullptr) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    DWORD high = 0;
    // GetFileSize answers failure with a sentinel *together with* the last
    // error, and Microsoft's documentation requires the error to be cleared
    // first: otherwise an error an earlier call left behind makes a good file
    // look like a failed one. On Windows 95 that value is arbitrary, so this is
    // the difference between reading an archive and reporting it missing.
    SetLastError(ERROR_SUCCESS);
    const DWORD low = GetFileSize(file, &high);
    if (low == INVALID_FILE_SIZE && GetLastError() != NO_ERROR)
        return FALSE;
    size->LowPart = low;
    size->HighPart = static_cast<LONG>(high);
    return TRUE;
}

OA_XP_DEFINE_SYSTEM(get_file_size_ex, GetFileSizeEx, 8);

/// Gives a name in the long form a program asks for.
///
/// Windows 95 OSR2 and every later Windows keep only long names, so a name is
/// already the long one asked for and is given back as it came.
///
/// @param name the name to read
/// @param[out] long_name the name, whose space is filled
/// @param capacity the characters long_name holds
/// @return the characters written without the terminator, or the characters
/// the name needs, terminator included, when capacity is too small for it
extern "C" DWORD WINAPI get_long_path_name(
    LPCWSTR name, LPWSTR long_name, DWORD capacity
) __asm__(OA_XP_SYSTEM_SYMBOL(GetLongPathNameW, 12));

DWORD WINAPI get_long_path_name(LPCWSTR name, LPWSTR long_name, DWORD capacity) {
    if (const auto system = system_get_long_path_name.get())
        return system(name, long_name, capacity);
    if (name == nullptr) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    const std::size_t length = std::wcslen(name);
    const DWORD needed = static_cast<DWORD>(length + 1);
    if (long_name == nullptr || capacity == 0)
        return needed;
    if (capacity < needed)
        return needed;
    std::memcpy(long_name, name, needed * sizeof(wchar_t));
    return static_cast<DWORD>(length);
}

OA_XP_DEFINE_SYSTEM(get_long_path_name, GetLongPathNameW, 12);

/// Takes the handle of the module a name or an address names.
///
/// Windows 95 has no reference counting to keep or to leave unchanged, so only
/// the address and the name are used.
///
/// @param flags GET_MODULE_HANDLE_EX_FLAG_ ones
/// @param name the module's name, or an address in it
/// @param[out] module the module's handle
/// @return true when the module was found
extern "C" BOOL WINAPI get_module_handle_ex(
    DWORD flags, LPCWSTR name, HMODULE* module
) __asm__(OA_XP_SYSTEM_SYMBOL(GetModuleHandleExW, 12));

BOOL WINAPI get_module_handle_ex(DWORD flags, LPCWSTR name, HMODULE* module) {
    if (const auto system = system_get_module_handle_ex.get())
        return system(flags, name, module);
    if (module == nullptr) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    HMODULE found = nullptr;
    if ((flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) != 0) {
        MEMORY_BASIC_INFORMATION mapped{};
        if (VirtualQuery(name, &mapped, sizeof(mapped)) == 0) {
            SetLastError(ERROR_MOD_NOT_FOUND);
            return FALSE;
        }
        found = static_cast<HMODULE>(mapped.AllocationBase);
    } else {
        found = GetModuleHandleW(name);
    }
    if (found == nullptr)
        return FALSE;
    *module = found;
    return TRUE;
}

OA_XP_DEFINE_SYSTEM(get_module_handle_ex, GetModuleHandleExW, 12);

/// Gives the language of the user's own interface.
///
/// Windows 95 draws its interface in the user's language, which is the one
/// this call gives.
///
/// @return the user's language
extern "C" LANGID WINAPI
get_user_default_ui_language(void) __asm__(
    OA_XP_SYSTEM_SYMBOL(GetUserDefaultUILanguage, 0)
);

LANGID WINAPI get_user_default_ui_language(void) {
    if (const auto system = system_get_user_default_ui_language.get())
        return system();
    return GetUserDefaultLangID();
}

OA_XP_DEFINE_SYSTEM(get_user_default_ui_language, GetUserDefaultUILanguage, 0);

/// Reads how much memory the machine has and how much is left.
///
/// Windows 95 answers for physical memory and for the swap file together and
/// for each separately, which is all the longer report holds.
///
/// @param[out] status the MEMORYSTATUSEX to fill, whose length the caller sets
/// @return true when the report was filled
extern "C" BOOL WINAPI
global_memory_status_ex(LPMEMORYSTATUSEX status) __asm__(
    OA_XP_SYSTEM_SYMBOL(GlobalMemoryStatusEx, 4)
);

BOOL WINAPI global_memory_status_ex(LPMEMORYSTATUSEX status) {
    if (const auto system = system_global_memory_status_ex.get())
        return system(status);
    if (status == nullptr || status->dwLength < sizeof(MEMORYSTATUSEX)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    MEMORYSTATUS plain{};
    plain.dwLength = sizeof(plain);
    GlobalMemoryStatus(&plain);
    status->dwMemoryLoad = plain.dwMemoryLoad;
    status->ullTotalPhys = plain.dwTotalPhys;
    status->ullAvailPhys = plain.dwAvailPhys;
    status->ullTotalPageFile = plain.dwTotalPageFile;
    status->ullAvailPageFile = plain.dwAvailPageFile;
    status->ullTotalVirtual = plain.dwTotalVirtual;
    status->ullAvailVirtual = plain.dwAvailVirtual;
    status->ullAvailExtendedVirtual = 0;
    return TRUE;
}

OA_XP_DEFINE_SYSTEM(global_memory_status_ex, GlobalMemoryStatusEx, 4);

/// Makes a lock ready for threads to take, with a count of times a thread
/// waits on the processor before it sleeps.
///
/// Windows 95's lock has no such count; every other part of making it ready is
/// the same, so the lock is ready and the count is not kept.
///
/// @param[out] section the lock to make ready
/// @param spin_count how many times a waiting thread spins first, ignored
/// @return true
extern "C" BOOL WINAPI initialize_critical_section_and_spin_count(
    LPCRITICAL_SECTION section, DWORD spin_count
) __asm__(OA_XP_SYSTEM_SYMBOL(InitializeCriticalSectionAndSpinCount, 8));

BOOL WINAPI
initialize_critical_section_and_spin_count(LPCRITICAL_SECTION section, DWORD spin_count) {
    if (const auto system = system_initialize_critical_section_and_spin_count.get())
        return system(section, spin_count);
    (void)spin_count;
    InitializeCriticalSection(section);
    return TRUE;
}

OA_XP_DEFINE_SYSTEM(
    initialize_critical_section_and_spin_count, InitializeCriticalSectionAndSpinCount, 8
);

/// Says whether a process runs as a 32-bit program on 64-bit Windows.
///
/// Windows 95 is not 64-bit and emulates no such program, so no process does.
///
/// @param process the process to ask about
/// @param[out] wow64 false
/// @return true when the answer was given
extern "C" BOOL WINAPI
is_wow64_process(HANDLE process, PBOOL wow64) __asm__(
    OA_XP_SYSTEM_SYMBOL(IsWow64Process, 8)
);

BOOL WINAPI is_wow64_process(HANDLE process, PBOOL wow64) {
    if (const auto system = system_is_wow64_process.get())
        return system(process, wow64);
    (void)process;
    if (wow64 == nullptr) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    *wow64 = FALSE;
    return TRUE;
}

OA_XP_DEFINE_SYSTEM(is_wow64_process, IsWow64Process, 8);

/// Moves a file's place to read and to write from.
///
/// Windows 95 moves it in two halves, the low one this call gives back and the
/// high one it writes beside it.
///
/// @param file the file to move in
/// @param distance how far to move
/// @param[out] position the place moved to, which the caller may pass as null
/// @param method where the move starts from, a FILE_BEGIN, FILE_CURRENT or
/// FILE_END
/// @return true when the file moved
extern "C" BOOL WINAPI set_file_pointer_ex(
    HANDLE file, LARGE_INTEGER distance, PLARGE_INTEGER position, DWORD method
) __asm__(OA_XP_SYSTEM_SYMBOL(SetFilePointerEx, 20));

BOOL WINAPI set_file_pointer_ex(
    HANDLE file, LARGE_INTEGER distance, PLARGE_INTEGER position, DWORD method
) {
    if (const auto system = system_set_file_pointer_ex.get())
        return system(file, distance, position, method);
    LARGE_INTEGER moved{};
    // The same clearing, for the same reason, before the call that answers
    // failure with a sentinel and the last error.
    SetLastError(ERROR_SUCCESS);
    moved.LowPart = SetFilePointer(file, distance.LowPart, &distance.HighPart, method);
    if (moved.LowPart == INVALID_SET_FILE_POINTER && GetLastError() != NO_ERROR)
        return FALSE;
    moved.HighPart = distance.HighPart;
    if (position != nullptr)
        *position = moved;
    return TRUE;
}

OA_XP_DEFINE_SYSTEM(set_file_pointer_ex, SetFilePointerEx, 20);

/// Keeps the machine awake, or lets it sleep, while the thread runs.
///
/// Windows 95 has no power management for a program to ask for, so nothing
/// sleeps that would not have; a caller that asked for the machine to stay
/// awake is told it does.
///
/// @param state the ES_ flags to set
/// @return the flags passed
extern "C" EXECUTION_STATE WINAPI
set_thread_execution_state(EXECUTION_STATE state) __asm__(
    OA_XP_SYSTEM_SYMBOL(SetThreadExecutionState, 4)
);

EXECUTION_STATE WINAPI set_thread_execution_state(EXECUTION_STATE state) {
    if (const auto system = system_set_thread_execution_state.get())
        return system(state);
    return state;
}

OA_XP_DEFINE_SYSTEM(set_thread_execution_state, SetThreadExecutionState, 4);

/// Takes a lock a thread holds, or finds that another thread does.
///
/// Windows 95's lock does not say whether it is free, but a lock no thread
/// holds holds a count of -1, which the one step below takes while leaving it
/// a word a second thread's step will not; a thread that already holds the
/// lock takes it again.
///
/// @param[in,out] section the lock to take
/// @return true when the calling thread now holds it
extern "C" BOOL WINAPI
try_enter_critical_section(LPCRITICAL_SECTION section) __asm__(
    OA_XP_SYSTEM_SYMBOL(TryEnterCriticalSection, 4)
);

BOOL WINAPI try_enter_critical_section(LPCRITICAL_SECTION section) {
    if (const auto system = system_try_enter_critical_section.get())
        return system(section);
    const HANDLE thread = reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(GetCurrentThreadId()));
    if (InterlockedCompareExchange(&section->LockCount, 0, -1) == -1) {
        section->RecursionCount = 1;
        section->OwningThread = thread;
        return TRUE;
    }
    if (section->OwningThread == thread) {
        InterlockedIncrement(&section->LockCount);
        ++section->RecursionCount;
        return TRUE;
    }
    return FALSE;
}

OA_XP_DEFINE_SYSTEM(try_enter_critical_section, TryEnterCriticalSection, 4);

namespace {

/// The word of a condition variable, whose count of wake-ups is what a waiter
/// waits for it to change.
uintptr_t& condition_word(void** condition) noexcept {
    return *reinterpret_cast<uintptr_t*>(condition);
}

/// Whether a condition has been woken since the caller noted its count.
bool condition_woken(void** condition, uintptr_t noted) noexcept {
    return std::atomic_ref<uintptr_t>(condition_word(condition)).load(std::memory_order_acquire)
        != noted;
}

/// How often a waiter spins, and then yields, before it sleeps.
constexpr uint32_t condition_spins = 8;
constexpr uint32_t condition_yields = 16;

} // namespace

/// Makes a condition that no thread waits on.
///
/// Windows 95 has no condition variable of the Windows API. The C++ run-time
/// library's thread support is told that the version compiled for has one
/// (cmake/toolchains/windows-95-gthr.hpp) and builds its condition variables
/// on these calls. The two that wake a waiter are oa-platform-xp-runtime's,
/// over the same word.
extern "C" void WINAPI initialize_condition_variable(void** condition) __asm__(
    OA_XP_SYSTEM_SYMBOL(InitializeConditionVariable, 4)
);

void WINAPI initialize_condition_variable(void** condition) {
    if (const auto system = system_initialize_condition_variable.get()) {
        system(condition);
        return;
    }
    condition_word(condition) = 0;
}

OA_XP_DEFINE_SYSTEM(initialize_condition_variable, InitializeConditionVariable, 4);

/// Releases the lock, waits for the condition to be woken, and takes the lock
/// again.
///
/// @return TRUE when woken, or FALSE with ERROR_TIMEOUT when the time ran out
extern "C" BOOL WINAPI sleep_condition_variable_cs(
    void** condition, void* section, DWORD timeout_ms
) __asm__(OA_XP_SYSTEM_SYMBOL(SleepConditionVariableCS, 12));

BOOL WINAPI sleep_condition_variable_cs(void** condition, void* section, DWORD timeout_ms) {
    if (const auto system = system_sleep_condition_variable.get())
        return system(condition, section, timeout_ms);
    // The count is noted before the lock is released, so a wake that comes
    // between the two is not lost.
    const uintptr_t noted =
        std::atomic_ref<uintptr_t>(condition_word(condition)).load(std::memory_order_acquire);
    auto* lock = static_cast<LPCRITICAL_SECTION>(section);
    LeaveCriticalSection(lock);
    const DWORD started = GetTickCount();
    uint32_t attempts = 0;
    bool woken = true;
    while (!condition_woken(condition, noted)) {
        if (timeout_ms != INFINITE && GetTickCount() - started >= timeout_ms) {
            woken = false;
            break;
        }
        if (attempts < condition_yields) {
            if (attempts++ >= condition_spins)
                Sleep(0);
            continue;
        }
        Sleep(1);
    }
    EnterCriticalSection(lock);
    if (!woken) {
        SetLastError(ERROR_TIMEOUT);
        return FALSE;
    }
    return TRUE;
}

OA_XP_DEFINE_SYSTEM(sleep_condition_variable_cs, SleepConditionVariableCS, 12);

/// Adds a condition to the mask VerifyVersionInfoW compares with.
///
/// A field's condition sits in the three bits at three times the field's
/// place, which is where the compare below reads it.
///
/// @param mask the mask so far
/// @param type the version field to set the condition for, one VER_ bit
/// @param condition how the field is to compare, one of the VER_ conditions
/// @return the mask with the condition added
extern "C" ULONGLONG WINAPI ver_set_condition_mask(
    ULONGLONG mask, DWORD type, BYTE condition
) __asm__(OA_XP_SYSTEM_SYMBOL(VerSetConditionMask, 16));

ULONGLONG WINAPI ver_set_condition_mask(ULONGLONG mask, DWORD type, BYTE condition) {
    if (const auto system = system_ver_set_condition_mask.get())
        return system(mask, type, condition);
    for (std::size_t field = 0; field < std::size(version_fields); ++field) {
        if (type == version_fields[field])
            return mask | (static_cast<ULONGLONG>(condition) << (field * 3));
    }
    return mask;
}

OA_XP_DEFINE_SYSTEM(ver_set_condition_mask, VerSetConditionMask, 16);

/// Says whether the running Windows meets a described one.
///
/// The description's every field the type names is compared with the running
/// version under the condition the mask holds for it.
///
/// @param wanted the version to compare with
/// @param type the fields to compare, VER_ bits together
/// @param mask the conditions, as ver_set_condition_mask built them
/// @return true when every field meets its condition
extern "C" BOOL WINAPI verify_version_info_w(
    LPOSVERSIONINFOEXW wanted, DWORD type, DWORDLONG mask
) __asm__(OA_XP_SYSTEM_SYMBOL(VerifyVersionInfoW, 16));

BOOL WINAPI verify_version_info_w(LPOSVERSIONINFOEXW wanted, DWORD type, DWORDLONG mask) {
    if (const auto system = system_verify_version_info.get())
        return system(wanted, type, mask);
    if (wanted == nullptr) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    // GetVersionExA: the narrow form is the one Windows 95 has, and the fields
    // compared here are numbers, so the version description's own strings — the
    // service pack name — are never read.
    OSVERSIONINFOEXA running{};
    running.dwOSVersionInfoSize = sizeof(running);
    if (GetVersionExA(reinterpret_cast<LPOSVERSIONINFOA>(&running)) == FALSE)
        return FALSE;
    for (std::size_t field = 0; field < std::size(version_fields); ++field) {
        if ((type & version_fields[field]) == 0)
            continue;
        const auto condition = static_cast<BYTE>((mask >> (field * 3)) & 0x7);
        if (!version_meets_condition(
                version_field_value(running, field), version_field_value(*wanted, field), condition
            )) {
            SetLastError(ERROR_OLD_WIN_VERSION);
            return FALSE;
        }
    }
    return TRUE;
}

OA_XP_DEFINE_SYSTEM(verify_version_info_w, VerifyVersionInfoW, 16);

// Windows 95 cannot open a directory with CreateFile: the call fails with
// ERROR_ACCESS_DENIED, because opening one is a capability Windows NT has and
// 95 has not, and FILE_FLAG_BACKUP_SEMANTICS is ignored there. mingw's
// std::filesystem stats a directory by opening it, so on this Windows
// fs::is_directory answers false for every directory that is there, and a
// program pointed at one concludes it does not exist.
//
// Opening a directory is answered here with a handle of this file's own.
// GetFileInformationByHandle fills it in from the folder's attributes, which
// is the field is_directory reads, and CloseHandle forgets it. Nothing else
// is done with the folder, which is all a caller that only asks about one
// does with it.

namespace {

/// Whether the path names a directory that is there.
bool names_a_directory(const char* narrow) noexcept {
    const DWORD attributes = GetFileAttributesA(narrow);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

/// A handle standing for an opened directory, and where it was.
struct DirectoryHandle {
    volatile LONG taken; ///< non-zero while this entry stands for one
    char path[MAX_PATH]{}; ///< the directory it was opened for
};

constexpr std::size_t directory_handle_count = 8;
constexpr uintptr_t directory_handle_base = 0x7f000000u;

DirectoryHandle directory_handles[directory_handle_count];

/// The handle value for an entry. Above every value the system gives out, so
/// no real handle can be mistaken for one.
void* directory_handle_for(std::size_t index) noexcept {
    return reinterpret_cast<void*>(directory_handle_base + index + 1);
}

/// The entry a handle names, or null when the handle is the system's.
DirectoryHandle* directory_handle_entry(void* handle) noexcept {
    const uintptr_t value = reinterpret_cast<uintptr_t>(handle);
    if (value <= directory_handle_base || value > directory_handle_base + directory_handle_count)
        return nullptr;
    DirectoryHandle& entry = directory_handles[value - directory_handle_base - 1];
    return entry.taken != 0 ? &entry : nullptr;
}

/// Opens a directory without the system, or returns null when it will not.
void* open_directory(const char* path) noexcept {
    for (std::size_t index = 0; index < directory_handle_count; ++index) {
        DirectoryHandle& entry = directory_handles[index];
        if (InterlockedCompareExchange(&entry.taken, 1, 0) != 0)
            continue;
        std::strncpy(entry.path, path, MAX_PATH - 1);
        entry.path[MAX_PATH - 1] = '\0';
        return directory_handle_for(index);
    }
    return nullptr;
}

/// A call of kernel32's, reached through the module handle in the system's own
/// character set. The SystemFunction lookups above go through
/// GetModuleHandleW, a stubbed wide call here, so they answer nothing on this
/// Windows and every stand-in below has to find the system's call itself.
void* kernel32_call(const char* name) noexcept {
    const HMODULE module = GetModuleHandleA("kernel32.dll");
    if (module == nullptr)
        return nullptr;
    return reinterpret_cast<void*>(GetProcAddress(module, name));
}

} // namespace

extern "C" HANDLE WINAPI create_file_a(
    LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES attributes, DWORD disposition,
    DWORD flags, HANDLE template_file
) __asm__(OA_XP_SYSTEM_SYMBOL(CreateFileA, 28));

/// Opens a file, or a directory where the system cannot open one.
HANDLE WINAPI create_file_a(
    LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES attributes, DWORD disposition,
    DWORD flags, HANDLE template_file
) {
    using Function =
        HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
    static const auto system = reinterpret_cast<Function>(kernel32_call("CreateFileA"));
    if (name != nullptr && disposition == OPEN_EXISTING && names_a_directory(name)) {
        if (void* handle = open_directory(name))
            return static_cast<HANDLE>(handle);
    }
    if (system == nullptr) {
        SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
        return INVALID_HANDLE_VALUE;
    }
    return system(name, access, share, attributes, disposition, flags, template_file);
}

OA_XP_DEFINE_SYSTEM(create_file_a, CreateFileA, 28);

extern "C" BOOL WINAPI get_file_information_by_handle(
    HANDLE file, LPBY_HANDLE_FILE_INFORMATION information
) __asm__(OA_XP_SYSTEM_SYMBOL(GetFileInformationByHandle, 8));

/// Reads what is known about a directory opened the way create_file_a opens
/// one, or asks the system about any other file.
BOOL WINAPI get_file_information_by_handle(HANDLE file, LPBY_HANDLE_FILE_INFORMATION information) {
    using Function = BOOL(WINAPI*)(HANDLE, LPBY_HANDLE_FILE_INFORMATION);
    static const auto system = reinterpret_cast<Function>(kernel32_call("GetFileInformationByHandle"));
    DirectoryHandle* const entry = directory_handle_entry(file);
    if (entry == nullptr) {
        if (system == nullptr) {
            SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
            return FALSE;
        }
        return system(file, information);
    }
    if (information == nullptr) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    WIN32_FIND_DATAA found{};
    const HANDLE search = FindFirstFileA(entry->path, &found);
    if (search == INVALID_HANDLE_VALUE) {
        const DWORD attributes = GetFileAttributesA(entry->path);
        if (attributes == INVALID_FILE_ATTRIBUTES)
            return FALSE;
        *information = BY_HANDLE_FILE_INFORMATION{};
        information->dwFileAttributes = attributes;
        return TRUE;
    }
    FindClose(search);
    *information = BY_HANDLE_FILE_INFORMATION{};
    information->dwFileAttributes = found.dwFileAttributes;
    information->ftCreationTime = found.ftCreationTime;
    information->ftLastAccessTime = found.ftLastAccessTime;
    information->ftLastWriteTime = found.ftLastWriteTime;
    information->nFileSizeHigh = found.nFileSizeHigh;
    information->nFileSizeLow = found.nFileSizeLow;
    return TRUE;
}

OA_XP_DEFINE_SYSTEM(get_file_information_by_handle, GetFileInformationByHandle, 8);

extern "C" BOOL WINAPI close_handle(HANDLE object) __asm__(OA_XP_SYSTEM_SYMBOL(CloseHandle, 4));

/// Closes a handle, forgetting one of this file's own.
BOOL WINAPI close_handle(HANDLE object) {
    using Function = BOOL(WINAPI*)(HANDLE);
    static const auto system = reinterpret_cast<Function>(kernel32_call("CloseHandle"));
    if (DirectoryHandle* const entry = directory_handle_entry(object)) {
        entry->path[0] = '\0';
        InterlockedExchange(&entry->taken, 0);
        return TRUE;
    }
    if (system == nullptr) {
        SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
        return FALSE;
    }
    return system(object);
}

OA_XP_DEFINE_SYSTEM(close_handle, CloseHandle, 4);

// The same for the wide call, which is the one the C++ run-time library's
// filesystem uses: it converts nothing itself, so a program that stats a
// folder goes through this name.
extern "C" HANDLE WINAPI create_file_w(
    LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES attributes, DWORD disposition,
    DWORD flags, HANDLE template_file
) __asm__(OA_XP_SYSTEM_SYMBOL(CreateFileW, 28));

/// Opens a file, or a directory where the system cannot open one.
///
/// This Windows answers the wide call with nothing: measured on the guest, it
/// returns NULL and leaves ERROR_CALL_NOT_IMPLEMENTED, and makes no file. NULL
/// is not the failure a caller is given to expect, which is
/// INVALID_HANDLE_VALUE, so a caller that checks for that one — as
/// preferences::save does — walks on holding a null handle and reports a later
/// step going wrong instead of this one.
///
/// So the path is read in the system's own character set and the narrow call is
/// made in its place, as every stand-in in this file does.
HANDLE WINAPI create_file_w(
    LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES attributes, DWORD disposition,
    DWORD flags, HANDLE template_file
) {
    char narrow[MAX_PATH + 1] = {};
    if (!narrow_path(name, narrow, sizeof(narrow))) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    return create_file_a(narrow, access, share, attributes, disposition, flags, template_file);
}

OA_XP_DEFINE_SYSTEM(create_file_w, CreateFileW, 28);

// MoveFileEx is not on this Windows either: measured on the guest, both forms
// answer with nothing (ERROR_CALL_NOT_IMPLEMENTED from each) while the plain
// MoveFile works. The behaviour it promises is therefore built here out of the
// calls that are there.
//
// MOVEFILE_REPLACE_EXISTING is the flag that matters and the one that must not
// be faked. MoveFile will not write over a file that is there — measured,
// ERROR_ALREADY_EXISTS, 183 — so the file that is there is removed first, and
// when that cannot be done the failure is reported rather than the move being
// allowed to appear to have worked and lose what was there.
//
// MOVEFILE_WRITE_THROUGH is not honoured: the old file is gone and the new one
// is in its place before this returns, but nothing here can promise more than
// the write that has already been done to the file. MOVEFILE_DELAY_UNTIL_REBOOT
// is refused outright, because the entry it needs cannot be written from here
// and doing the move now would be a different thing from what was asked for.

extern "C" BOOL WINAPI move_file_ex_w(LPCWSTR from, LPCWSTR to, DWORD flags) __asm__(
    OA_XP_SYSTEM_SYMBOL(MoveFileExW, 12)
);

BOOL WINAPI move_file_ex_w(LPCWSTR from, LPCWSTR to, DWORD flags) {
    char source[MAX_PATH + 1] = {};
    char destination[MAX_PATH + 1] = {};
    if (!narrow_path(from, source, sizeof(source)) ||
        !narrow_path(to, destination, sizeof(destination))) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if ((flags & MOVEFILE_DELAY_UNTIL_REBOOT) != 0) {
        SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
        return FALSE;
    }
    const bool replace = (flags & MOVEFILE_REPLACE_EXISTING) != 0;
    if (replace) {
        SetLastError(ERROR_SUCCESS);
        if (!DeleteFileA(destination)) {
            // A destination that is not there is what a save normally has.
            const DWORD error = GetLastError();
            if (error != ERROR_SUCCESS && error != ERROR_FILE_NOT_FOUND &&
                error != ERROR_PATH_NOT_FOUND)
                return FALSE;
        }
    }
    if (MoveFileA(source, destination))
        return TRUE;
    const DWORD moved_error = GetLastError();
    // A move between two volumes is what COPY_ALLOWED is for, and a copy with
    // the original removed leaves the same thing behind.
    if ((flags & MOVEFILE_COPY_ALLOWED) != 0) {
        SetLastError(ERROR_SUCCESS);
        if (CopyFileA(source, destination, replace ? FALSE : TRUE) && DeleteFileA(source))
            return TRUE;
    }
    SetLastError(moved_error);
    return FALSE;
}

OA_XP_DEFINE_SYSTEM(move_file_ex_w, MoveFileExW, 12);

// The wide semaphore and event, which this Windows answers with nothing as it
// does the other wide calls. SDL makes one of each to start its video, so
// without these a program cannot open a window at all: the start stops with
// "Couldn't create semaphore" and no frame is ever shown.

using WideCreateSemaphoreFunction = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, LONG, LONG, LPCWSTR);
using WideCreateEventFunction = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, BOOL, BOOL, LPCWSTR);
constinit SystemFunction<WideCreateSemaphoreFunction> system_wide_create_semaphore{
    kernel32, "CreateSemaphoreW"
};
constinit SystemFunction<WideCreateEventFunction> system_wide_create_event{
    kernel32, "CreateEventW"
};

extern "C" HANDLE WINAPI create_semaphore_w(
    LPSECURITY_ATTRIBUTES attributes, LONG initial, LONG maximum, LPCWSTR name
) __asm__(OA_XP_SYSTEM_SYMBOL(CreateSemaphoreW, 16));

/// Creates a semaphore, as CreateSemaphoreW does, through the narrow call this
/// Windows does answer.
///
/// The name is the only part of the call that a character set belongs to, so
/// it is narrowed and the rest is passed on unchanged. A null name — an
/// unnamed semaphore — stays null.
///
/// @param attributes the security attributes, or null for the default ones
/// @param initial the count the semaphore starts with
/// @param maximum the largest count it may reach
/// @param name the name to create it under, or null for an unnamed semaphore
/// @return the semaphore, or null with the error set as the narrow call sets it
HANDLE WINAPI create_semaphore_w(
    LPSECURITY_ATTRIBUTES attributes, LONG initial, LONG maximum, LPCWSTR name
) {
    if (const auto system = system_wide_create_semaphore.get())
        return system(attributes, initial, maximum, name);
    char narrow[MAX_PATH + 1]{};
    const char* named = nullptr;
    if (name != nullptr) {
        if (WideCharToMultiByte(CP_ACP, 0, name, -1, narrow, sizeof(narrow), nullptr, nullptr) == 0) {
            SetLastError(ERROR_INVALID_NAME);
            return nullptr;
        }
        named = narrow;
    }
    return CreateSemaphoreA(attributes, initial, maximum, named);
}

OA_XP_DEFINE_SYSTEM(create_semaphore_w, CreateSemaphoreW, 16);

extern "C" HANDLE WINAPI create_event_w(
    LPSECURITY_ATTRIBUTES attributes, BOOL manual_reset, BOOL initial, LPCWSTR name
) __asm__(OA_XP_SYSTEM_SYMBOL(CreateEventW, 16));

/// Creates an event, as CreateEventW does, through the narrow call this
/// Windows does answer, for the same reason as the semaphore above: SDL makes
/// one to be woken by, and without it the video start fails.
///
/// @param attributes the security attributes, or null for the default ones
/// @param manual_reset whether the event stays set until it is reset by hand
/// @param initial whether it starts set
/// @param name the name to create it under, or null for an unnamed event
/// @return the event, or null with the error set as the narrow call sets it
HANDLE WINAPI create_event_w(
    LPSECURITY_ATTRIBUTES attributes, BOOL manual_reset, BOOL initial, LPCWSTR name
) {
    if (const auto system = system_wide_create_event.get())
        return system(attributes, manual_reset, initial, name);
    char narrow[MAX_PATH + 1]{};
    const char* named = nullptr;
    if (name != nullptr) {
        if (WideCharToMultiByte(CP_ACP, 0, name, -1, narrow, sizeof(narrow), nullptr, nullptr) == 0) {
            SetLastError(ERROR_INVALID_NAME);
            return nullptr;
        }
        named = narrow;
    }
    return CreateEventA(attributes, manual_reset, initial, named);
}

OA_XP_DEFINE_SYSTEM(create_event_w, CreateEventW, 16);

/// The code a program raises to hand a debugger the name of the thread raising
/// it. It is not a fault, and the name it carries is a courtesy to a debugger.
constexpr DWORD thread_name_exception = 0x406d1388;

extern "C" void WINAPI raise_exception(
    DWORD code, DWORD flags, DWORD argument_count, const ULONG_PTR* arguments
) __asm__(OA_XP_SYSTEM_SYMBOL(RaiseException, 16));

/// Raises an exception, as RaiseException does, except for the one a program
/// raises to name the thread it is running on.
///
/// That call is not a fault being reported: it is a program handing a debugger
/// the name of the thread, which a debugger catches, reads and ignores. SDL
/// raises it for every thread it names, and installs a vectored handler to
/// take it back — and a vectored handler is a Windows XP addition this Windows
/// cannot install, so here the raise is read by no one and the thread dies on
/// it, before a window can be shown, saying nothing about why. A name meant
/// for a debugger is nothing a system without one can lose, so this one is
/// dropped; every other code is raised as before, the C++ run-time library's
/// own among them.
///
/// @param code the exception code
/// @param flags whether the exception may be continued
/// @param argument_count how many arguments the exception carries
/// @param arguments those arguments
void WINAPI raise_exception(
    DWORD code, DWORD flags, DWORD argument_count, const ULONG_PTR* arguments
) {
    if (code == thread_name_exception)
        return;
    using Function = void(WINAPI*)(DWORD, DWORD, DWORD, const ULONG_PTR*);
    static const auto system = reinterpret_cast<Function>(kernel32_call("RaiseException"));
    if (system == nullptr) {
        SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
        return;
    }
    system(code, flags, argument_count, arguments);
}

OA_XP_DEFINE_SYSTEM(raise_exception, RaiseException, 16);
