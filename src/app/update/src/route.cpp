// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The route for this machine. macOS answers a signed bundle. The self-check
// folder is answered on every platform.
#include "oa/app/update/route.hpp"

#include "oa/app/version_line.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
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
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <mach-o/dyld.h>
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

/// True when a folder's status says its owner may write it.
///
/// @param folder the folder
/// @return true when the status names a writable directory
[[nodiscard]] bool folder_can_be_written(const fs::path& folder) {
    std::error_code error;
    const fs::file_status status = fs::status(folder, error);
    if (error || !fs::is_directory(status))
        return false;
    const fs::perms bits = status.permissions();
    return (bits & fs::perms::owner_write) != fs::perms::none;
}

#if defined(__APPLE__)
/// The signing team of this process. Empty when the signature names none,
/// which is an ad hoc signature.
///
/// @return the team identifier
[[nodiscard]] std::string signing_team() {
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
    const std::string spelling = executable.generic_string();
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
    if (facts.platform == "macos-universal")
        return macos_route(facts);
    where.why = "no route for this platform yet";
    return where;
}

RouteFacts this_machine(const fs::path& program_folder) {
    RouteFacts facts;
    facts.executable = executable_path();
    facts.program_folder = program_folder;
    facts.folder_writable = folder_can_be_written(program_folder);
    facts.platform = std::string(release_platform_name());
    read_bundle(facts.executable, facts);
#if defined(OA_SELF_CHECKS) && OA_SELF_CHECKS
    facts.self_check = true;
#else
    facts.self_check = false;
#endif
    return facts;
}

} // namespace oa::app::update
