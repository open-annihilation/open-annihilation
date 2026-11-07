// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Windows: the shell recycles a file. A file the Recycle Bin cannot take
// stays where it is, and nobody is asked. Before Windows 8 the shell cannot be
// told to recycle only, so a file that is not on a fixed drive is left before
// the shell is asked. Windows 9x has no such call that this build can make, so
// the move fails and the file stays.

#include "oa/platform/trash.hpp"

#include "oa/platform/files.hpp"

#include <filesystem>
#include <string>
#include <system_error>

#include <windows.h>
#include <shellapi.h>

#if defined(NTDDI_VERSION) && NTDDI_VERSION >= 0x06000000
#include <objbase.h>
#include <shobjidl.h>
#define OA_RECYCLE_ONLY 1
#ifndef FOFX_RECYCLEONDELETE
#define FOFX_RECYCLEONDELETE 0x00080000
#endif
#endif

namespace oa::platform {
namespace {

/// The path the shell is given: a full path, two nulls at the end, as
/// SHFileOperationW asks for a list.
///
/// @param file the file
/// @return the list; empty when the path has no spelling
std::wstring shell_path(const std::filesystem::path& file) {
    std::error_code failure;
    std::filesystem::path full = file;
    if (full.is_relative())
        full = std::filesystem::absolute(full, failure);
    if (failure)
        return {};
    const std::u16string wide = utf16_from_utf8(path_to_utf8(full));
    if (wide.empty())
        return {};
    std::wstring listed;
    listed.reserve(wide.size() + 1);
    for (const char16_t unit : wide)
        listed.push_back(static_cast<wchar_t>(unit));
    listed.push_back(L'\0');
    return listed;
}

/// Why a file is left before the shell is asked.
constexpr const char* no_recycle_bin = "the Recycle Bin cannot take a file on this drive";

/// Tells whether a file is on a fixed drive.
///
/// Network shares and removable drives have no Recycle Bin, and a shell asked
/// to delete with undo there deletes the file for good without a question.
/// GetVolumePathNameW is looked up when it is used: Windows 95 does not have
/// it, and a file there counts as not on a fixed drive.
///
/// @param listed the path, as shell_path spells it
/// @return true when the system says the file's volume is a fixed drive
bool on_fixed_drive(const std::wstring& listed) noexcept {
    using VolumePathName = BOOL(WINAPI*)(LPCWSTR, LPWSTR, DWORD);
    const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    const auto volume_path_name =
        kernel != nullptr
            ? reinterpret_cast<VolumePathName>(
                  reinterpret_cast<void*>(GetProcAddress(kernel, "GetVolumePathNameW"))
              )
            : nullptr;
    wchar_t root[MAX_PATH + 1]{};
    if (volume_path_name == nullptr || volume_path_name(listed.c_str(), root, MAX_PATH) == 0)
        return false;
    return GetDriveTypeW(root) == DRIVE_FIXED;
}

#ifdef OA_RECYCLE_ONLY

/// Tells whether this is Windows 8 or later, where the shell can be told to
/// recycle only.
///
/// The system's own version, which a compatibility manifest does not change.
/// A version that cannot be read counts as earlier.
///
/// @return true from Windows 8 (6.2) on
bool windows_8_or_later() noexcept {
    using VersionFunction = LONG(WINAPI*)(OSVERSIONINFOW*);
    // What the version reader returns when it read the version.
    constexpr LONG version_read = 0;
    const HMODULE system_library = GetModuleHandleW(L"ntdll.dll");
    const auto read_version =
        system_library != nullptr
            ? reinterpret_cast<VersionFunction>(
                  reinterpret_cast<void*>(GetProcAddress(system_library, "RtlGetVersion"))
              )
            : nullptr;
    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof version;
    if (read_version == nullptr || read_version(&version) != version_read)
        return false;
    return version.dwMajorVersion > 6 ||
           (version.dwMajorVersion == 6 && version.dwMinorVersion >= 2);
}

/// The system's own words for a failure, with the trailing break stripped.
///
/// @param code the failure
/// @return the words; empty when the system has none
std::string system_message(HRESULT code) {
    wchar_t* buffer = nullptr;
    const DWORD wrote = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        static_cast<DWORD>(code),
        0,
        reinterpret_cast<wchar_t*>(&buffer),
        0,
        nullptr
    );
    if (wrote == 0 || buffer == nullptr)
        return {};
    std::u16string wide;
    wide.reserve(wrote);
    for (DWORD index = 0; index < wrote; ++index)
        wide.push_back(static_cast<char16_t>(buffer[index]));
    LocalFree(buffer);
    while (!wide.empty() && (wide.back() == u'\n' || wide.back() == u'\r' || wide.back() == u' '))
        wide.pop_back();
    return utf8_from_utf16(wide);
}

/// Asks the shell to recycle one file, and only to recycle it.
///
/// FOF_ALLOWUNDO recycles. From Windows 8 on, FOFX_RECYCLEONDELETE also
/// leaves a file the Recycle Bin cannot take. On Vista and 7, which do not
/// know that flag, a file that is not on a fixed drive is left before the
/// shell is asked. No confirmation is asked. The system's own words are kept
/// only when it reports a failure: an operation it reports as done but
/// aborted has none.
///
/// @param listed the path, as shell_path spells it
/// @param[out] out the result
void recycle_only(const std::wstring& listed, TrashMove& out) {
    const bool recycle_only_known = windows_8_or_later();
    if (!recycle_only_known && !on_fixed_drive(listed)) {
        out.error = no_recycle_bin;
        return;
    }
    const HRESULT started = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool own_apartment = started == S_OK;
    if (FAILED(started) && started != RPC_E_CHANGED_MODE) {
        out.error = system_message(started);
        return;
    }
    IFileOperation* operation = nullptr;
    const HRESULT created = CoCreateInstance(
        CLSID_FileOperation,
        nullptr,
        CLSCTX_ALL,
        IID_IFileOperation,
        reinterpret_cast<void**>(&operation)
    );
    if (FAILED(created) || operation == nullptr) {
        out.error = system_message(created);
        if (own_apartment)
            CoUninitialize();
        return;
    }
    DWORD flags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    if (recycle_only_known)
        flags |= FOFX_RECYCLEONDELETE;
    HRESULT code = operation->SetOperationFlags(flags);
    IShellItem* item = nullptr;
    if (SUCCEEDED(code)) {
        code = SHCreateItemFromParsingName(
            listed.c_str(), nullptr, IID_IShellItem, reinterpret_cast<void**>(&item)
        );
    }
    if (SUCCEEDED(code) && item != nullptr)
        code = operation->DeleteItem(item, nullptr);
    if (SUCCEEDED(code))
        code = operation->PerformOperations();
    BOOL aborted = FALSE;
    if (SUCCEEDED(code)) {
        const HRESULT asked = operation->GetAnyOperationsAborted(&aborted);
        if (FAILED(asked))
            code = asked;
    }
    if (item != nullptr)
        item->Release();
    operation->Release();
    if (own_apartment)
        CoUninitialize();
    if (FAILED(code)) {
        out.error = system_message(code);
        return;
    }
    // Aborted with no failure: the system gave no reason, and "The operation
    // completed successfully." is not one.
    if (aborted)
        return;
    out.moved = true;
    out.place = TrashPlace::trash;
}

#endif

} // namespace

bool system_has_trash() noexcept {
    return true;
}

TrashMove move_to_trash(const std::filesystem::path& file) {
    TrashMove out;
    std::error_code present;
    if (!std::filesystem::is_regular_file(file, present) || present) {
        out.error = "the file is not there";
        return out;
    }
    const std::wstring listed = shell_path(file);
    if (listed.empty()) {
        out.error = "the file's path cannot be written";
        return out;
    }
#ifdef OA_RECYCLE_ONLY
    recycle_only(listed, out);
    if (out.moved && std::filesystem::is_regular_file(file, present) && !present) {
        out.moved = false;
        out.place = TrashPlace::trash;
    }
    return out;
#else
    // Before Vista, and on Windows 9x where this call does not recycle, the
    // shell is asked to recycle without a window. A failure leaves the file.
    // FOF_WANTNUKEWARNING is not set: it would ask once per file, with no
    // owner window, when the Recycle Bin cannot take it. Without it the shell
    // deletes such a file for good, so a file that is not on a fixed drive (a
    // network share, a removable drive) is left before the shell is asked.
    if (!on_fixed_drive(listed)) {
        out.error = no_recycle_bin;
        return out;
    }
    SHFILEOPSTRUCTW operation{};
    operation.wFunc = FO_DELETE;
    operation.pFrom = listed.c_str();
    operation.fFlags =
        static_cast<FILEOP_FLAGS>(FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT);
    const int code = SHFileOperationW(&operation);
    if (code != 0 || operation.fAnyOperationsAborted) {
        out.error = "the trash did not take the file (" + std::to_string(code) + ")";
        return out;
    }
    if (std::filesystem::is_regular_file(file, present) && !present) {
        out.error = "the trash did not take the file";
        return out;
    }
    out.moved = true;
    out.place = TrashPlace::trash;
    return out;
#endif
}

} // namespace oa::platform
