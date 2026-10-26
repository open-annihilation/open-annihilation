// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The route for this machine. Only the self-check folder is answered here.
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
    where.why = "no route for this platform yet";
    return where;
}

RouteFacts this_machine(const fs::path& program_folder) {
    RouteFacts facts;
    facts.executable = executable_path();
    facts.program_folder = program_folder;
    facts.folder_writable = folder_can_be_written(program_folder);
    facts.platform = std::string(release_platform_name());
    return facts;
}

} // namespace oa::app::update
