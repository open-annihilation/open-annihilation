// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The wide C run-time functions that Windows 95 does not export and that the
// C++ run-time library calls, defined under the names a program imports them
// by. Windows 95 exports the wide half of the C run-time surface as stubs that
// fail, and has no size-suffixed variants of it at all, so each of these asks
// the system's own system-character-set function instead and widens or narrows
// around it. Built only for MinGW, whose import naming it follows.
//
// The macro and SystemFunction definitions below are oa-platform-xp-runtime's
// windows_functions.cpp's; the two modules share no header, deliberately (see
// ../README.md).
// The Windows Vista and 7 functions, and the newer C library functions,
// that the C++ run-time library calls and Windows XP lacks,
// defined under the names a program imports them by. A program linked with
// this file calls these in place of the system's; each uses the system's own
// function when the running Windows has it and a stand-in when it does not.
// Built only for MinGW, whose import naming it follows.

// The C++ standard headers come first, before the Windows XP declarations are
// pinned below: they reach the toolchain's thread support, whose condition
// variables a win32-threaded toolchain declares for the version this file is
// compiled at, not for Windows XP.
#include <atomic>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <sys/utime.h>
#include <cwctype>

// The declarations of Windows XP, so that none of the functions defined here
// is also declared as the system's; and the module list of the process-status
// library, which this file's own module list falls back to.
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

#include <psapi.h>

// The symbol a function is defined by, and the import pointer a program
// calls it through, in MinGW's naming: on 32-bit x86 a leading underscore,
// and for the system's functions the size of their arguments.
#if defined(__i386__)
#define OA_XP_SYSTEM_SYMBOL(name, argument_bytes) "_" #name "@" #argument_bytes
#define OA_XP_SYSTEM_IMPORT(name, argument_bytes) "__imp__" #name "@" #argument_bytes
#define OA_XP_LIBRARY_SYMBOL(name) "_" #name
#define OA_XP_LIBRARY_IMPORT(name) "__imp__" #name
#else
#define OA_XP_SYSTEM_SYMBOL(name, argument_bytes) #name
#define OA_XP_SYSTEM_IMPORT(name, argument_bytes) "__imp_" #name
#define OA_XP_LIBRARY_SYMBOL(name) #name
#define OA_XP_LIBRARY_IMPORT(name) "__imp_" #name
#endif

// Defines `function`, declared before with its type, as the system function
// `name` and as the import pointer a program calls `name` through.
#define OA_XP_DEFINE_SYSTEM(function, name, argument_bytes)                                        \
    extern "C" constinit decltype(&function)                                                       \
        const function##_import __asm__(OA_XP_SYSTEM_IMPORT(name, argument_bytes)) = &function
#define OA_XP_DEFINE_LIBRARY(function, name)                                                       \
    extern "C" constinit decltype(&function)                                                       \
        const function##_import __asm__(OA_XP_LIBRARY_IMPORT(name)) = &function

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
                found = reinterpret_cast<Function>(
                    reinterpret_cast<void*>(GetProcAddress(module, name))
                );
            function.store(found, std::memory_order_relaxed);
            looked_up.store(true, std::memory_order_release);
        }
        return function.load(std::memory_order_relaxed);
    }
};

extern "C" errno_t __cdecl local_time_32(struct tm* out, const __time32_t* timer) __asm__(
    OA_XP_LIBRARY_SYMBOL(_localtime32_s)
);

/// Breaks a 32-bit time down into the local time it names, as _localtime32_s
/// does. The stand-in has the system's localtime fill it in; the C library
/// Windows 95 ships has no secure form of its own.
///
/// @param[out] out receives the broken-down time
/// @param timer seconds since the epoch
/// @return 0, or EINVAL for a null argument or a time localtime refuses
errno_t __cdecl local_time_32(struct tm* out, const __time32_t* timer) {
    if (out == nullptr || timer == nullptr)
        return EINVAL;
    const time_t value = static_cast<time_t>(*timer);
    const struct tm* broken_down = std::localtime(&value);
    if (broken_down == nullptr)
        return EINVAL;
    *out = *broken_down;
    return 0;
}

OA_XP_DEFINE_LIBRARY(local_time_32, _localtime32_s);

// The header names the wide stat calls as macros pointing at a time_t
// variant, and the symbols below are taken from the names as written.
#undef _wstat64
#undef _wstat

extern "C" int __cdecl
wide_stat_64(const wchar_t* path, struct _stat64* buffer) __asm__(OA_XP_LIBRARY_SYMBOL(_wstat64));

/// Stats a path, as _wstat64 does, through the narrow _stat64 the C library
/// Windows 95 ships answers correctly.
///
/// Windows 95's _wstat64 reports every path as absent — a file that is there
/// and a directory that is there alike answer -1 — while its narrow _stat64
/// answers correctly, down to the _S_IFDIR bit and the size. The C++
/// run-time library's std::filesystem stats through _wstat64, so on that
/// system is_directory and exists are false for a directory that is there,
/// and a program asking whether its game folder exists is told it does not.
/// Narrowing the path and asking _stat64 is the whole of the difference: the
/// two agree on the layout of the structure.
///
/// @param path the path to stat
/// @param[out] buffer receives what is known about the path
/// @return 0, or -1 with errno set as _stat64 sets it
int __cdecl wide_stat_64(const wchar_t* path, struct _stat64* buffer) {
    if (path == nullptr || buffer == nullptr) {
        errno = EINVAL;
        return -1;
    }
    char narrow[1024]{};
    const int written = WideCharToMultiByte(
        CP_ACP, 0, path, -1, narrow, static_cast<int>(sizeof(narrow)), nullptr, nullptr
    );
    if (written <= 0) {
        errno = ENOENT;
        return -1;
    }
    return _stat64(narrow, buffer);
}

OA_XP_DEFINE_LIBRARY(wide_stat_64, _wstat64);

/// Widens what a narrow search found into the wide structure the wide calls
/// answer with. The two structures hold the same fields in the same order and
/// differ only in the character set of the name, so everything but the name is
/// copied across and the name is widened over.
///
/// @param[out] wide receives the widened match
/// @param narrow what the narrow call found
void wide_find_data(struct _wfinddata_t* wide, const struct _finddata_t& narrow) {
    wide->attrib = narrow.attrib;
    wide->time_create = narrow.time_create;
    wide->time_access = narrow.time_access;
    wide->time_write = narrow.time_write;
    wide->size = narrow.size;
    MultiByteToWideChar(CP_ACP, 0, narrow.name, -1, wide->name, MAX_PATH);
}

// Named as the header names them, for the same reason as the stat calls above.
#undef _wfindfirst32
#undef _wfindnext32
#undef _wfindfirst
#undef _wfindnext

extern "C" long __cdecl wide_find_first(const wchar_t* pattern, struct _wfinddata_t* data) __asm__(
    OA_XP_LIBRARY_SYMBOL(_wfindfirst32)
);

/// Begins a search, as _wfindfirst does, through the narrow _findfirst the C
/// library Windows 95 ships answers correctly.
///
/// Windows 95's _wfindfirst fails with EINVAL for every pattern, so the C++
/// run-time library's directory_iterator — which lists a folder through it —
/// reports "Invalid argument" and a program reading a folder is told the
/// folder cannot be listed even though it is there and its entries are
/// reachable through the narrow call. The search handle the narrow call gives
/// back is used unchanged by _wfindnext below, which is why the two are
/// stood in for together.
///
/// @param pattern the search pattern, as the wide call takes it
/// @param[out] data receives the first match, widened
/// @return a search handle, or -1 with errno set as _findfirst sets it
long __cdecl wide_find_first(const wchar_t* pattern, struct _wfinddata_t* data) {
    if (pattern == nullptr || data == nullptr) {
        errno = EINVAL;
        return -1;
    }
    char narrow[1024]{};
    if (WideCharToMultiByte(
            CP_ACP, 0, pattern, -1, narrow, static_cast<int>(sizeof(narrow)), nullptr, nullptr
        ) <= 0) {
        errno = EINVAL;
        return -1;
    }
    struct _finddata_t found{};
    const long handle = _findfirst(narrow, &found);
    if (handle == -1) {
        if (errno == ENOENT)
            SetLastError(ERROR_FILE_NOT_FOUND);
        return -1;
    }
    wide_find_data(data, found);
    return handle;
}

OA_XP_DEFINE_LIBRARY(wide_find_first, _wfindfirst32);

extern "C" int __cdecl
wide_find_next(long handle, struct _wfinddata_t* data) __asm__(OA_XP_LIBRARY_SYMBOL(_wfindnext32));

/// Continues a search, as _wfindnext does, through the narrow _findnext, on
/// the handle the stand-in above gave back.
///
/// The Windows error is set alongside errno when the search runs out, because
/// the C++ run-time library's directory stream tells the end of a listing from
/// a failure by reading it: the narrow call reports the end through errno
/// alone, so without this a folder read to its last entry is reported as one
/// that cannot be read, and a program listing its game folder is told the
/// folder does not exist.
///
/// @param handle a handle from _wfindfirst
/// @param[out] data receives the next match, widened
/// @return 0 when another was found, -1 with errno set as _findnext sets it
int __cdecl wide_find_next(long handle, struct _wfinddata_t* data) {
    if (data == nullptr) {
        errno = EINVAL;
        return -1;
    }
    struct _finddata_t found{};
    if (_findnext(handle, &found) != 0) {
        if (errno == ENOENT)
            SetLastError(ERROR_NO_MORE_FILES);
        return -1;
    }
    wide_find_data(data, found);
    return 0;
}

OA_XP_DEFINE_LIBRARY(wide_find_next, _wfindnext32);

/// Narrows a path into the buffer a narrow call is given.
///
/// @param[out] narrow receives the path in the system's own character set
/// @param capacity bytes narrow holds
/// @param wide the path to narrow
/// @return true when the system wrote it and it fitted
bool narrow_path(char* narrow, size_t capacity, const wchar_t* wide) noexcept {
    if (narrow == nullptr || wide == nullptr)
        return false;
    return WideCharToMultiByte(
               CP_ACP, 0, wide, -1, narrow, static_cast<int>(capacity), nullptr, nullptr
           ) > 0;
}

// The header names the wide opens beside the narrow ones they are built from.
#undef _wopen
#undef _wfopen
#undef _wfreopen

extern "C" int __cdecl
wide_open(const wchar_t* path, int flags, ...) __asm__(OA_XP_LIBRARY_SYMBOL(_wopen));

/// Opens a file, as _wopen does, through the narrow _open the C library
/// Windows 95 ships answers correctly.
///
/// Windows 95's _wopen fails with EBADF for every path — one that is there and
/// one that is not alike — while its narrow _open opens the same file. The C++
/// run-time library opens every file through it, so on that system no archive
/// of the game's can be opened, and a program reading its game data is told
/// each archive cannot be opened while the files sit there readable.
///
/// The sharing mode is only passed on when it was given: _open takes it with
/// _O_CREAT and not otherwise.
///
/// @param path the file to open
/// @param flags the open flags, as _open takes them
/// @param ... the sharing mode, which _open takes only with _O_CREAT
/// @return the file descriptor, or -1 with errno set as _open sets it
int __cdecl wide_open(const wchar_t* path, int flags, ...) {
    char narrow[1024]{};
    if (!narrow_path(narrow, sizeof(narrow), path)) {
        errno = ENOENT;
        return -1;
    }
    if ((flags & _O_CREAT) != 0) {
        va_list arguments;
        va_start(arguments, flags);
        const int permissions = va_arg(arguments, int);
        va_end(arguments);
        return _open(narrow, flags, permissions);
    }
    return _open(narrow, flags);
}

OA_XP_DEFINE_LIBRARY(wide_open, _wopen);

extern "C" FILE* __cdecl
wide_fopen(const wchar_t* path, const wchar_t* mode) __asm__(OA_XP_LIBRARY_SYMBOL(_wfopen));

/// Opens a stream on a file, as _wfopen does, through the narrow fopen, for the
/// same reason as the wide open above: the wide one is a stub here and the
/// narrow one is not.
///
/// @param path the file to open
/// @param mode the mode to open it with, widened by the caller
/// @return the stream, or null with errno set as fopen sets it
FILE* __cdecl wide_fopen(const wchar_t* path, const wchar_t* mode) {
    char narrow[1024]{};
    char narrow_mode[16]{};
    if (!narrow_path(narrow, sizeof(narrow), path) ||
        !narrow_path(narrow_mode, sizeof(narrow_mode), mode)) {
        errno = ENOENT;
        return nullptr;
    }
    return std::fopen(narrow, narrow_mode);
}

OA_XP_DEFINE_LIBRARY(wide_fopen, _wfopen);

extern "C" FILE* __cdecl wide_freopen(
    const wchar_t* path, const wchar_t* mode, FILE* stream
) __asm__(OA_XP_LIBRARY_SYMBOL(_wfreopen));

/// Reopens a stream on another file, as _wfreopen does, through the narrow
/// freopen, for the same reason again.
///
/// @param path the file to open the stream on
/// @param mode the mode to open it with
/// @param stream the stream to reopen
/// @return the stream, or null with errno set as freopen sets it
FILE* __cdecl wide_freopen(const wchar_t* path, const wchar_t* mode, FILE* stream) {
    char narrow[1024]{};
    char narrow_mode[16]{};
    if (!narrow_path(narrow, sizeof(narrow), path) ||
        !narrow_path(narrow_mode, sizeof(narrow_mode), mode)) {
        errno = EINVAL;
        return nullptr;
    }
    return std::freopen(narrow, narrow_mode, stream);
}

OA_XP_DEFINE_LIBRARY(wide_freopen, _wfreopen);

// The wide directory and timestamp calls, which Windows 95 refuses outright.
// Its narrow twins answer, so each of these narrows the path and asks the
// narrow one, as the wide open and the wide stat above do. The header names
// the wide beside the narrow they are built from.
#undef _wmkdir
#undef _wchdir
#undef _wchmod
#undef _wgetcwd
#undef _wutime

extern "C" int __cdecl wide_mkdir(const wchar_t* path) __asm__(OA_XP_LIBRARY_SYMBOL(_wmkdir));

/// Makes a directory, as _wmkdir does, through the narrow _mkdir the C library
/// Windows 95 ships answers correctly.
///
/// Windows 95's wide directory calls answer -1 with errno EINVAL for every
/// path — a directory to make in a folder right there, and a folder that is
/// already there, alike — while their narrow twins answer as they should. The
/// C++ run-time library makes every directory through _wmkdir, so on that
/// system std::filesystem::create_directories throws "cannot create
/// directories: Invalid argument" and a program that lays down its own folder
/// tree cannot start. Narrowing the path is the whole of the difference.
///
/// @param path the directory to make
/// @return 0, or -1 with errno set as _mkdir sets it
int __cdecl wide_mkdir(const wchar_t* path) {
    char narrow[1024]{};
    if (!narrow_path(narrow, sizeof(narrow), path)) {
        errno = EINVAL;
        return -1;
    }
    return _mkdir(narrow);
}

OA_XP_DEFINE_LIBRARY(wide_mkdir, _wmkdir);

extern "C" int __cdecl wide_chdir(const wchar_t* path) __asm__(OA_XP_LIBRARY_SYMBOL(_wchdir));

/// Makes a directory the current one, as _wchdir does, through the narrow
/// _chdir, for the same reason as wide_mkdir above.
///
/// @param path the directory to make current
/// @return 0, or -1 with errno set as _chdir sets it
int __cdecl wide_chdir(const wchar_t* path) {
    char narrow[1024]{};
    if (!narrow_path(narrow, sizeof(narrow), path)) {
        errno = EINVAL;
        return -1;
    }
    return _chdir(narrow);
}

OA_XP_DEFINE_LIBRARY(wide_chdir, _wchdir);

extern "C" int __cdecl
wide_chmod(const wchar_t* path, int mode) __asm__(OA_XP_LIBRARY_SYMBOL(_wchmod));

/// Sets a path's permission bits, as _wchmod does, through the narrow _chmod,
/// for the same reason as wide_mkdir above.
///
/// @param path the path to set the bits of
/// @param mode the bits to set
/// @return 0, or -1 with errno set as _chmod sets it
int __cdecl wide_chmod(const wchar_t* path, int mode) {
    char narrow[1024]{};
    if (!narrow_path(narrow, sizeof(narrow), path)) {
        errno = EINVAL;
        return -1;
    }
    return _chmod(narrow, mode);
}

OA_XP_DEFINE_LIBRARY(wide_chmod, _wchmod);

extern "C" int __cdecl
wide_utime(const wchar_t* path, struct _utimbuf* times) __asm__(OA_XP_LIBRARY_SYMBOL(_wutime));

/// Sets a path's times, as _wutime does, through the narrow _utime, for the
/// same reason as wide_mkdir above. The two take the same structure.
///
/// @param path the path to set the times of
/// @param times the times to set, or null for the present time
/// @return 0, or -1 with errno set as _utime sets it
int __cdecl wide_utime(const wchar_t* path, struct _utimbuf* times) {
    char narrow[1024]{};
    if (!narrow_path(narrow, sizeof(narrow), path)) {
        errno = EINVAL;
        return -1;
    }
    return _utime(narrow, times);
}

OA_XP_DEFINE_LIBRARY(wide_utime, _wutime);

extern "C" wchar_t* __cdecl
wide_getcwd(wchar_t* buffer, int maxlen) __asm__(OA_XP_LIBRARY_SYMBOL(_wgetcwd));

/// Gives the current directory, as _wgetcwd does, from the narrow _getcwd, for
/// the same reason as wide_mkdir above — and here the wide call answers NULL
/// for a directory that is plainly current.
///
/// A caller that gives no buffer is given an allocated path, which it frees,
/// so the path is widened into memory the C library's own allocation holds.
///
/// @param buffer where the path goes, or null to have one allocated
/// @param maxlen how many characters buffer holds, unread when it is null
/// @return buffer, the allocated path, or null with errno set
wchar_t* __cdecl wide_getcwd(wchar_t* buffer, int maxlen) {
    char narrow[1024]{};
    if (_getcwd(narrow, static_cast<int>(sizeof(narrow))) == nullptr)
        return nullptr;
    if (buffer == nullptr) {
        const int needed = MultiByteToWideChar(CP_ACP, 0, narrow, -1, nullptr, 0);
        if (needed <= 0) {
            errno = EINVAL;
            return nullptr;
        }
        auto* wide =
            static_cast<wchar_t*>(std::malloc(sizeof(wchar_t) * static_cast<size_t>(needed)));
        if (wide == nullptr) {
            errno = ENOMEM;
            return nullptr;
        }
        MultiByteToWideChar(CP_ACP, 0, narrow, -1, wide, needed);
        return wide;
    }
    if (maxlen <= 0) {
        errno = EINVAL;
        return nullptr;
    }
    if (MultiByteToWideChar(CP_ACP, 0, narrow, -1, buffer, maxlen) <= 0) {
        errno = ERANGE;
        return nullptr;
    }
    return buffer;
}

OA_XP_DEFINE_LIBRARY(wide_getcwd, _wgetcwd);

} // namespace
