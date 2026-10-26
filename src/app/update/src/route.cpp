// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The route for this machine. The self-check folder and Windows are answered.
#include "oa/app/update/route.hpp"

#include "oa/app/version_line.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winreg.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace oa::app::update {
namespace {

namespace fs = std::filesystem;

/// The file this process was started from.
///
/// @return the executable; empty when the system will not name it
[[nodiscard]] fs::path executable_path() {
#if defined(_WIN32)
    std::wstring wide(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length =
            GetModuleFileNameW(nullptr, wide.data(), static_cast<DWORD>(wide.size()));
        if (length == 0) {
            std::string narrow(MAX_PATH, '\0');
            const DWORD narrow_length =
                GetModuleFileNameA(nullptr, narrow.data(), static_cast<DWORD>(narrow.size()));
            if (narrow_length == 0 || narrow_length >= narrow.size())
                return {};
            narrow.resize(narrow_length);
            return fs::path(narrow);
        }
        if (length < wide.size()) {
            wide.resize(length);
            return fs::path(wide);
        }
        if (wide.size() >= 32768)
            return {};
        wide.resize(wide.size() * 2);
    }
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(static_cast<std::size_t>(size) + 1U, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0)
        return {};
    return fs::path(buffer.data());
#else
    std::error_code error;
    const fs::path resolved = fs::read_symlink("/proc/self/exe", error);
    return error ? fs::path() : resolved;
#endif
}

/// True when a scratch file can be created in `folder` and then deleted.
///
/// @param folder the folder
/// @return true when the scratch file was written
[[nodiscard]] bool folder_can_be_written(const fs::path& folder) {
    std::error_code error;
    if (!fs::is_directory(folder, error) || error)
        return false;
    const fs::path scratch = folder / "oa-write-check.tmp";
    {
        std::ofstream out(scratch, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out.put('x');
        if (!out)
            return false;
    }
    fs::remove(scratch, error);
    return true;
}

#if defined(_WIN32)
/// The uninstall key, without a 64-bit view flag, so this process sees its own view.
constexpr std::string_view uninstall_key =
    "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Open Annihilation";

/// UTF-8 for a wide registry value, with trailing NULs removed.
[[nodiscard]] std::string utf8_from_wide(std::wstring_view wide) {
    while (!wide.empty() && wide.back() == L'\0')
        wide.remove_suffix(1);
    if (wide.empty())
        return {};
    const int needed = WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr
    );
    if (needed <= 0)
        return {};
    std::string utf8(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        wide.data(),
        static_cast<int>(wide.size()),
        utf8.data(),
        needed,
        nullptr,
        nullptr
    );
    return utf8;
}

/// UTF-8 for an ANSI registry value. The wide call is used when it exists.
[[nodiscard]] std::string utf8_from_acp(std::string_view acp) {
    while (!acp.empty() && acp.back() == '\0')
        acp.remove_suffix(1);
    if (acp.empty())
        return {};
    const int wide_needed =
        MultiByteToWideChar(CP_ACP, 0, acp.data(), static_cast<int>(acp.size()), nullptr, 0);
    if (wide_needed <= 0)
        return std::string(acp);
    std::wstring wide(static_cast<std::size_t>(wide_needed), L'\0');
    MultiByteToWideChar(
        CP_ACP, 0, acp.data(), static_cast<int>(acp.size()), wide.data(), wide_needed
    );
    return utf8_from_wide(wide);
}

/// InstallLocation from the wide registry, or empty when the value is not a string.
[[nodiscard]] std::string query_location_wide(HKEY key) {
    DWORD type = 0;
    DWORD bytes = 0;
    const LONG sized = RegQueryValueExW(key, L"InstallLocation", nullptr, &type, nullptr, &bytes);
    if (sized != ERROR_SUCCESS || type != REG_SZ || bytes < sizeof(wchar_t))
        return {};
    std::wstring wide(bytes / sizeof(wchar_t), L'\0');
    DWORD got = bytes;
    const LONG read = RegQueryValueExW(
        key, L"InstallLocation", nullptr, &type, reinterpret_cast<BYTE*>(wide.data()), &got
    );
    if (read != ERROR_SUCCESS || type != REG_SZ)
        return {};
    return utf8_from_wide(wide);
}

/// InstallLocation. The wide call is tried first. Windows 9x's stub falls back to ANSI.
[[nodiscard]] std::string install_location_from_registry() {
    const std::wstring wide_key(uninstall_key.begin(), uninstall_key.end());
    HKEY opened = nullptr;
    const LONG opened_wide =
        RegOpenKeyExW(HKEY_LOCAL_MACHINE, wide_key.c_str(), 0, KEY_READ, &opened);
    if (opened_wide == ERROR_SUCCESS) {
        const std::string value = query_location_wide(opened);
        RegCloseKey(opened);
        return value;
    }
    if (opened_wide != ERROR_CALL_NOT_IMPLEMENTED)
        return {};
    HKEY opened_ansi = nullptr;
    const LONG opened_narrow = RegOpenKeyExA(
        HKEY_LOCAL_MACHINE, std::string(uninstall_key).c_str(), 0, KEY_READ, &opened_ansi
    );
    if (opened_narrow != ERROR_SUCCESS)
        return {};
    DWORD type = 0;
    DWORD bytes = 0;
    const LONG sized =
        RegQueryValueExA(opened_ansi, "InstallLocation", nullptr, &type, nullptr, &bytes);
    if (sized != ERROR_SUCCESS || type != REG_SZ || bytes == 0) {
        RegCloseKey(opened_ansi);
        return {};
    }
    std::string acp(static_cast<std::size_t>(bytes), '\0');
    DWORD got = bytes;
    const LONG read = RegQueryValueExA(
        opened_ansi, "InstallLocation", nullptr, &type, reinterpret_cast<BYTE*>(acp.data()), &got
    );
    RegCloseKey(opened_ansi);
    if (read != ERROR_SUCCESS || type != REG_SZ)
        return {};
    return utf8_from_acp(acp);
}
#endif

/// A location folded the way `same_install_folder` compares them.
[[nodiscard]] std::string normalised_install(std::string_view text) {
    std::string cleaned;
    cleaned.reserve(text.size());
    for (const char character : text) {
        if (character == '"')
            continue;
        cleaned.push_back(character == '\\' ? '/' : character);
    }
    if (cleaned.empty())
        return {};
    std::string normal = fs::path(cleaned).lexically_normal().generic_string();
    const bool drive_root = normal.size() == 3 && normal[1] == ':' && normal[2] == '/';
    if (normal.size() > 1 && normal.back() == '/' && !drive_root)
        normal.pop_back();
    for (char& character : normal) {
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character - 'A' + 'a');
    }
    return normal;
}

/// The program folder as UTF-8 with forward slashes.
[[nodiscard]] std::string folder_spelling(const fs::path& folder) {
    const std::u8string text = folder.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

} // namespace

Where find_route(const ThisBuild& build, const RouteFacts& facts) {
    Where where;
    if (facts.way == oa::data::engine_feed::Way::page_only) {
        where.why = "the release is offered on its page";
        return where;
    }
    if (build.development && !facts.test_feed) {
        where.why = "a development build has no route";
        return where;
    }
    if (facts.install_folder.has_value() && !facts.install_folder->empty()) {
        where.route = Route::check_folder;
        where.install = *facts.install_folder;
        where.program = facts.executable;
        return where;
    }
    if (facts.platform.starts_with("windows-")) {
        where.program = facts.executable;
        where.install = facts.program_folder;
        const std::string folder = folder_spelling(facts.program_folder);
        if (facts.uninstall_beside && same_install_folder(facts.install_location, folder))
            where.route = Route::windows_installer;
        else
            where.route = Route::windows_folder;
        return where;
    }
    where.why = "no route for this platform yet";
    return where;
}

RouteFacts this_machine(const fs::path& program_folder) {
    RouteFacts facts;
    facts.executable = executable_path();
    facts.program_folder = program_folder;
    facts.folder_writable = folder_can_be_written(program_folder);
    std::error_code error;
    facts.uninstall_beside = fs::is_regular_file(program_folder / "Uninstall.exe", error) && !error;
#if defined(_WIN32)
    facts.install_location = install_location_from_registry();
#endif
    facts.platform = std::string(release_platform_name());
    return facts;
}

bool same_install_folder(std::string_view left, std::string_view right) {
    if (left.empty() || right.empty())
        return false;
    const std::string normalised_left = normalised_install(left);
    const std::string normalised_right = normalised_install(right);
    return !normalised_left.empty() && normalised_left == normalised_right;
}

} // namespace oa::app::update
