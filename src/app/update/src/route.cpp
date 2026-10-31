// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The route for this machine. The self-check folder, Windows and Linux are
// answered, and macOS answers a signed bundle.
#include "oa/app/update/route.hpp"

#include "oa/app/version_line.hpp"
#include "oa/platform/files.hpp"
#include "oa/platform/system.hpp"

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
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <TargetConditionals.h>
#include <mach-o/dyld.h>
#include <unistd.h>
#endif
#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
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
    std::string normal = oa::platform::path_to_generic_utf8(
        oa::platform::path_from_utf8(cleaned).lexically_normal()
    );
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

#if defined(__APPLE__)
/// The signing team of this process. Empty when the signature names none,
/// which is an ad hoc signature.
///
/// @return the team identifier
[[nodiscard]] std::string signing_team() {
#if TARGET_OS_OSX
    SecCodeRef own = nullptr;
    if (SecCodeCopySelf(kSecCSDefaultFlags, &own) != errSecSuccess || own == nullptr)
        return {};
    SecStaticCodeRef disk_code = nullptr;
    const OSStatus made = SecCodeCopyStaticCode(own, kSecCSDefaultFlags, &disk_code);
    CFRelease(own);
    if (made != errSecSuccess || disk_code == nullptr)
        return {};
    CFDictionaryRef information = nullptr;
    const OSStatus copied =
        SecCodeCopySigningInformation(disk_code, kSecCSSigningInformation, &information);
    CFRelease(disk_code);
    if (copied != errSecSuccess || information == nullptr)
        return {};
    std::string team;
    const void* const value = CFDictionaryGetValue(information, kSecCodeInfoTeamIdentifier);
    if (value != nullptr && CFGetTypeID(value) == CFStringGetTypeID()) {
        const auto text = static_cast<CFStringRef>(value);
        const char* const direct = CFStringGetCStringPtr(text, kCFStringEncodingUTF8);
        if (direct != nullptr) {
            team = direct;
        } else {
            const CFIndex length = CFStringGetLength(text);
            const CFIndex bytes =
                CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
            std::string buffer(static_cast<std::size_t>(bytes), '\0');
            if (CFStringGetCString(text, buffer.data(), bytes, kCFStringEncodingUTF8))
                team.assign(buffer.c_str());
        }
    }
    CFRelease(information);
    return team;
#else
    // Code signing information is macOS's: an iOS copy installs from its
    // store and is never replaced in place.
    return {};
#endif
}

/// True when this process may write `folder`.
///
/// @param folder the folder that holds the bundle
/// @return true when access allows writing
[[nodiscard]] bool directory_writable(const fs::path& folder) {
    if (folder.empty())
        return false;
    return access(folder.c_str(), W_OK) == 0;
}
#endif

/// The bundle around `executable`, when it sits in Contents/MacOS.
///
/// @param executable the program file
/// @param[out] facts the bundle, the protected place, the team and the folder
void read_bundle(const fs::path& executable, RouteFacts& facts) {
#if defined(__APPLE__)
    const std::string spelling = oa::platform::path_to_generic_utf8(executable);
    facts.translocated = spelling.find("/AppTranslocation/") != std::string::npos;
    const fs::path folder = executable.parent_path();
    if (folder.filename() != "MacOS" || folder.parent_path().filename() != "Contents")
        return;
    facts.bundle = folder.parent_path().parent_path();
    facts.signing_team = signing_team();
    facts.parent_writable = directory_writable(facts.bundle.parent_path());
#else
    (void)executable;
    (void)facts;
#endif
}

/// The macOS route for a bundle the facts already describe.
///
/// @param facts the machine
/// @return the route, or why there is none
[[nodiscard]] Where macos_route(const RouteFacts& facts) {
    Where where;
    if (facts.bundle.empty()) {
        where.why = "no route for this platform yet";
        return where;
    }
    if (facts.translocated) {
        where.why = "macOS is running this copy from a protected place";
        return where;
    }
    const bool ad_hoc = facts.signing_team.empty();
    if (ad_hoc && !(facts.self_check && facts.test_feed)) {
        where.why = "an ad hoc signed build does not update in place";
        return where;
    }
    where.route = Route::macos_bundle;
    where.install = facts.bundle;
    where.program = facts.executable;
    where.installer = !facts.parent_writable;
    return where;
}

/// A path as UTF-8 with forward slashes.
///
/// @param path the path
/// @return its spelling
std::string generic_utf8(const fs::path& path) {
    const std::u8string text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

/// True when `folder` is `root` or a folder inside it. `root` ends with a slash.
///
/// @param folder the install, as generic UTF-8
/// @param root the blocked prefix, with its trailing slash
/// @return true for `/opt` and `/opt/oa`, false for `/optima`
bool under_prefix(std::string_view folder, std::string_view root) {
    const std::string_view exact = root.substr(0, root.size() - 1);
    return folder == exact || folder.starts_with(root);
}

/// True when the install is one the player cannot replace.
///
/// @param install the install folder
/// @return true under /opt, /usr, /snap or /var/lib/flatpak
bool system_location(const fs::path& install) {
    const std::string folder = generic_utf8(install);
    return under_prefix(folder, "/opt/") || under_prefix(folder, "/usr/") ||
           under_prefix(folder, "/snap/") || under_prefix(folder, "/var/lib/flatpak/");
}

/// The install folder. A versions layout is the folder above `versions/`.
///
/// @param facts the machine's facts
/// @return the folder the route replaces
fs::path install_root(const RouteFacts& facts) {
    const fs::path parent = facts.program_folder.parent_path();
    if (parent.filename() == "versions" && facts.launcher_present)
        return parent.parent_path();
    return facts.program_folder;
}

/// True for the three Linux package platforms.
///
/// @param platform the package platform name
/// @return true for linux-x86_64, linux-arm64 and linux-armhf
bool linux_platform(std::string_view platform) {
    return platform == "linux-x86_64" || platform == "linux-arm64" || platform == "linux-armhf";
}

/// True when `path` is a regular file or a link to one, without requiring the target.
///
/// @param path the path
/// @return true when a launcher file is there
bool launcher_file(const fs::path& path) {
    std::error_code error;
    const fs::file_status status = fs::symlink_status(path, error);
    return !error && (fs::is_regular_file(status) || fs::is_symlink(status));
}

} // namespace

Where find_route(const ThisBuild& build, const RouteFacts& facts) {
    Where where;
    if (facts.no_install_route) {
        where.why = "this check has no install route";
        return where;
    }
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
    if (facts.platform == "macos-universal")
        return macos_route(facts);
    if (facts.platform.starts_with("windows-")) {
        where.program = facts.executable;
        where.install = facts.program_folder;
        const std::string folder = folder_spelling(facts.program_folder);
        if (facts.uninstall_beside && same_install_folder(facts.install_location, folder))
            where.route = Route::windows_installer;
        else
            where.route = Route::windows_folder;
        // The installer route downloads and runs the setup program.
        where.installer = where.route == Route::windows_installer;
        return where;
    }
    if (!linux_platform(facts.platform)) {
        where.why = "no route for this platform yet";
        return where;
    }
    const fs::path install = install_root(facts);
    if (!facts.owned_by_player || facts.appimage || facts.flatpak || system_location(install)) {
        where.why = std::string(whole_system_copy);
        return where;
    }
    where.route = Route::linux_versions;
    where.install = install;
    where.program = facts.executable;
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
    read_bundle(facts.executable, facts);
#if defined(OA_SELF_CHECKS) && OA_SELF_CHECKS
    facts.self_check = true;
#else
    facts.self_check = false;
#endif
    facts.owned_by_player = true;
#if !defined(_WIN32)
    struct stat info{};
    if (::stat(program_folder.c_str(), &info) == 0)
        facts.owned_by_player = info.st_uid == ::geteuid();
#endif
    const std::optional<std::string> appimage = oa::platform::environment_value("APPIMAGE");
    const std::optional<std::string> flatpak = oa::platform::environment_value("FLATPAK_ID");
    facts.appimage = appimage.has_value();
    facts.flatpak = flatpak.has_value();
    if (program_folder.parent_path().filename() == "versions") {
        const fs::path launcher = program_folder.parent_path().parent_path() / "open-annihilation";
        facts.launcher_present = launcher_file(launcher);
    }
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
