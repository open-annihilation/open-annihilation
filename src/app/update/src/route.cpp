// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The route for this machine. Only the self-check folder is answered here.
#include "oa/app/update/route.hpp"

#include "oa/app/version_line.hpp"
#include "oa/platform/system.hpp"

#include <cstdint>
#include <filesystem>
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
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
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
    facts.platform = std::string(release_platform_name());
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

} // namespace oa::app::update
